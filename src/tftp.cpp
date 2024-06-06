#include "../inc/tftp.hpp"
#include <istream>      // for std::istream
#include <cstring>
#include <cassert>
#include <iostream>
// #include <future>
#include <mutex>
#include <thread>
#include <queue>

using namespace tftpc;

//std::future<std::streamsize> asyncRead(std::istream& stream, uint8_t* buffer, std::streamsize size) {
//	return std::async(std::launch::async, [&stream, buffer, size] {
//		stream.read(reinterpret_cast<char*>(buffer), size);
//		return stream.gcount();
//		});
//}

void strncpy_inc_offset(uint8_t* buffer, const char* str, size_t len, uint16_t& offset) {
	strncpy(reinterpret_cast<char*>(buffer + offset), str, len);
	offset += len;
	buffer[offset++] = '\0';
}

uint32_t getOsError() {
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

std::streamsize getStreamLength(std::istream& stream) {
	std::streamsize current = stream.tellg();
	stream.seekg(0, std::ios::end);
	std::streamsize length = stream.tellg();

	if (length <= 0) {
		throw TftpError(TftpError::ErrorType::IO, getOsError(), "Failed to get stream length");
	}

	stream.seekg(current, std::ios::beg);
	return length;
}

void Client::send(const struct sockaddr_in& remote_addr, const std::string& filename, std::istream& data) {
	LARGE_INTEGER frequency;
	LARGE_INTEGER start;
	LARGE_INTEGER end;
	QueryPerformanceFrequency(&frequency);


	std::streamsize length = getStreamLength(data);		// todo - does this allways work?

	/* local address & socket setup */
	struct sockaddr_in local_addr = {};
	local_addr.sin_family = AF_INET;
	local_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	local_addr.sin_port = htons(0);
	size_t local_addr_len = sizeof(local_addr);

#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to initialize Winsock");
	}
#endif

	socket_t sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to create socket");

	// close socket and WSACleanup on exception
	CleanupGuard guard(sockfd);

	if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to bind socket");

	uint8_t buffer[Config::BlockSize] = { 0 };
	uint8_t recv_buffer[Config::BlockSize] = { 0 };
	uint16_t buffer_offset = 2;
	int32_t recv_offset = -1;
	buffer[1] = static_cast<uint8_t>(TftpOpcode::WriteRequest);

	uint16_t blksize_val = Config::BlockSize;
	std::string blksize_str = std::to_string(blksize_val);

	std::string tsize_str = std::to_string(length);
	std::cout << "tsize: " << tsize_str << ", blksize: " << blksize_str << std::endl;

	strncpy_inc_offset(buffer, filename.c_str(), filename.size(), buffer_offset);
	strncpy_inc_offset(buffer, "octet", 5, buffer_offset);

	strncpy_inc_offset(buffer, "tsize", 5, buffer_offset);
	strncpy_inc_offset(buffer, tsize_str.c_str(), tsize_str.size(), buffer_offset);

	strncpy_inc_offset(buffer, "blksize", 7, buffer_offset);
	strncpy_inc_offset(buffer, blksize_str.c_str(), blksize_str.size(), buffer_offset);

	// timeout - TODO

	if (sendto(sockfd, reinterpret_cast<char*>(buffer), buffer_offset, 0, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send request");

	struct sockaddr_in comm_addr = {};
	socklen_t comm_addr_len = sizeof(comm_addr);

	if ((recv_offset = recvfrom(sockfd, (char*)(recv_buffer), Config::BlockSize, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive response");
	if (recv_offset < 4) throw TftpError(TftpError::ErrorType::Tftp, 0, "Invalid response");

	switch (recv_buffer[1]) {
	case static_cast<uint8_t>(TftpOpcode::Oack):
		break;
	case static_cast<uint8_t>(TftpOpcode::Ack):
		blksize_val = 512;
		break;
	case static_cast<uint8_t>(TftpOpcode::Error):
		throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), reinterpret_cast<char*>(recv_buffer + 4));
	default:
		throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
	}

	uint16_t block_num = 1;
	char data_header[4] = { 0, static_cast<uint8_t>(TftpOpcode::Data), 0, 0 };
	std::queue<std::unique_ptr<std::vector<uint8_t>>> data_queue;
	std::mutex data_queue_mutex;

	// data chunker thread:
	std::thread data_chunker([&data, &data_queue, &data_queue_mutex, blksize_val] {
		std::unique_ptr<std::vector<uint8_t>> data_chunk = std::make_unique<std::vector<uint8_t>>(blksize_val);
		while (data.read(reinterpret_cast<char*>(data_chunk->data()), blksize_val)) {
			std::lock_guard<std::mutex> lock(data_queue_mutex);
			data_queue.push(std::move(data_chunk));
			data_chunk = std::make_unique<std::vector<uint8_t>>(blksize_val);
		}
		data_chunk->resize(data.gcount());
		std::lock_guard<std::mutex> lock(data_queue_mutex);
		data_queue.push(std::move(data_chunk));
		});

	guard.guardThread(std::move(data_chunker));

	// data sender loop:
	std::this_thread::sleep_for(std::chrono::milliseconds(2));	// so there is time for the data_chunker to add at least one chunk

	double interval;

	double packet_construction_time = 0;
	double send_time = 0;
	double recv_time = 0;

	do {
		QueryPerformanceCounter(&start);
		std::unique_ptr<std::vector<uint8_t>> data_chunk = nullptr;
		{
			std::lock_guard<std::mutex> lock(data_queue_mutex);
			data_chunk = std::move(data_queue.front());
			data_queue.pop();
		}

		buffer_offset = data_chunk->size();

		data_header[2] = block_num >> 8;
		data_header[3] = block_num & 0xFF;

		WSABUF packet[2];
		packet[0].buf = data_header;
		packet[0].len = 4;
		packet[1].buf = reinterpret_cast<char*>(data_chunk->data());
		packet[1].len = buffer_offset;

		QueryPerformanceCounter(&end);
		packet_construction_time += (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
		QueryPerformanceCounter(&start);

		DWORD bytes_sent;
		DWORD flags = 0;

		if (WSASendTo(sockfd, packet, 2, &bytes_sent, flags, (struct sockaddr*)&comm_addr, comm_addr_len, nullptr, nullptr) == SOCKET_ERROR)
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send data");

		QueryPerformanceCounter(&end);
		send_time += (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;

		QueryPerformanceCounter(&start);

		// todo - wrong block number, timeout catching, etc.
		if ((recv_offset = recvfrom(sockfd, reinterpret_cast<char*>(recv_buffer), Config::BlockSize, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 0)
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive response");
		// if (recv_offset < 4) throw TftpError(TftpError::ErrorType::Tftp, 0, "Invalid response");

		switch (recv_buffer[1]) {
		case static_cast<uint8_t>(TftpOpcode::Ack):
			block_num++;
			break;
		case static_cast<uint8_t>(TftpOpcode::Error):
			throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), reinterpret_cast<char*>(recv_buffer + 4));
		default:
			throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
		}

		QueryPerformanceCounter(&end);
		recv_time += (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
	} while (buffer_offset == blksize_val && !data_queue.empty());

	std::cout << "Packet construction time: " << packet_construction_time << "s" << std::endl;
	std::cout << "Send time: " << send_time << "s" << std::endl;
	std::cout << "Recv time: " << recv_time << "s" << std::endl;

	// cleanup left to guard
}
