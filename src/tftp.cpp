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

	DWORD timeout = Config::Timeout * 1000;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to set socket timeout");

	uint8_t buffer[Config::BlockSize] = { 0 };
	uint8_t recv_buffer[Config::BlockSize] = { 0 };
	uint16_t buffer_offset = 2;
	int32_t recv_offset = -1;
	buffer[1] = static_cast<uint8_t>(TftpOpcode::WriteRequest);

	uint16_t blksize_val = Config::BlockSize;
	std::string blksize_str = std::to_string(blksize_val);
	std::string tsize_str = std::to_string(length);

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

	/* Absolutely no performance improvements over simple memcpy loop, but it looks fancier */

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
	while (data_queue.size() == 0) continue;

	int retries = Config::MaxRetries;

	do {
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

	resend_data_packet:
		DWORD bytes_sent;
		DWORD flags = 0;

		if (WSASendTo(sockfd, packet, 2, &bytes_sent, flags, (struct sockaddr*)&comm_addr, comm_addr_len, nullptr, nullptr) == SOCKET_ERROR)
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send data");

		if ((recv_offset = recvfrom(sockfd, reinterpret_cast<char*>(recv_buffer), Config::BlockSize, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 4) {
			auto errn = getOsError();

			switch (errn) {
			case WSAETIMEDOUT:
				retries--;
				if (retries == 0) throw TftpError(TftpError::ErrorType::Tftp, 0, "Max retries exceeded");
				goto resend_data_packet;
			default:
				throw TftpError(TftpError::ErrorType::OS, errn, "Failed to receive response");
			}
		}

		switch (recv_buffer[1]) {
		case static_cast<uint8_t>(TftpOpcode::Ack): {
			uint16_t block_num_ack = (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF);
			if (block_num_ack != block_num) {
				retries--;
				if (retries == 0) throw TftpError(TftpError::ErrorType::Tftp, 0, "Max retries exceeded");
				goto resend_data_packet;
			}
			block_num++;
		} break;
		case static_cast<uint8_t>(TftpOpcode::Error):
			throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), reinterpret_cast<char*>(recv_buffer + 4));
		default:
			throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
		}

	} while (buffer_offset == blksize_val && !data_queue.empty());

	// cleanup left to the guard
}

std::streamsize Client::recv(const struct sockaddr_in& remote_addr, const std::string& filename, std::ostream& data) {
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

	DWORD timeout = Config::Timeout * 1000;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to set socket timeout");

	uint8_t buffer[Config::BlockSize + 4] = { 0 };
	uint8_t recv_buffer[Config::BlockSize + 4] = { 0 };
	uint16_t buffer_offset = 2;
	int32_t recv_offset = -1;
	buffer[1] = static_cast<uint8_t>(TftpOpcode::ReadRequest);

	uint16_t blksize_val = Config::BlockSize;
	std::string blksize_str = std::to_string(blksize_val);
	std::string tsize_str = std::to_string(0);

	strncpy_inc_offset(buffer, filename.c_str(), filename.size(), buffer_offset);
	strncpy_inc_offset(buffer, "octet", 5, buffer_offset);

	strncpy_inc_offset(buffer, "blksize", 7, buffer_offset);
	strncpy_inc_offset(buffer, blksize_str.c_str(), blksize_str.size(), buffer_offset);

	strncpy_inc_offset(buffer, "tsize", 5, buffer_offset);
	strncpy_inc_offset(buffer, tsize_str.c_str(), tsize_str.size(), buffer_offset);

	if (sendto(sockfd, reinterpret_cast<char*>(buffer), buffer_offset, 0, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send request");

	struct sockaddr_in comm_addr = {};
	socklen_t comm_addr_len = sizeof(comm_addr);

	if ((recv_offset = recvfrom(sockfd, (char*)(recv_buffer), Config::BlockSize, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 4)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive a valid response");

	uint8_t ack_buffer[4] = { 0, static_cast<uint8_t>(TftpOpcode::Ack), 0, 0 };
	uint16_t block_num = 1;
	std::streamsize total_size = 0;

	switch (recv_buffer[1]) {
	case static_cast<uint8_t>(TftpOpcode::Oack):
		break;
	case static_cast<uint8_t>(TftpOpcode::Data): {
		uint16_t recv_blknum = (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF);
		if (recv_blknum != 1)
			throw TftpError(TftpError::ErrorType::Tftp, recv_blknum, "Invalid block number");

		blksize_val = recv_offset - 4;
		data.write(reinterpret_cast<char*>(recv_buffer + 4), blksize_val);
		ack_buffer[3] = recv_buffer[3];
		block_num++;
		total_size += blksize_val;
		goto skip_zeroAck;
	}
	case static_cast<uint8_t>(TftpOpcode::Error):
		throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), reinterpret_cast<char*>(recv_buffer + 4));
	default:
		throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
	}

	if (sendto(sockfd, reinterpret_cast<char*>(ack_buffer), 4, 0, (struct sockaddr*)&comm_addr, comm_addr_len) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send ack");

skip_zeroAck:

	int retries = Config::MaxRetries;

	do {
		if ((recv_offset = recvfrom(sockfd, reinterpret_cast<char*>(recv_buffer), Config::BlockSize + 4, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 4) {
			auto errn = getOsError();

			switch (errn) {
			case WSAETIMEDOUT:
				retries--;
				if (retries == 0) throw TftpError(TftpError::ErrorType::Tftp, 0, "Max retries exceeded");
				continue;
			default:
				throw TftpError(TftpError::ErrorType::OS, errn, "Failed to receive response");
			}
		}

		switch (recv_buffer[1]) {
		case static_cast<uint8_t>(TftpOpcode::Data): {
			uint16_t recv_blknum = (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF);
			if (recv_blknum != block_num) {
				retries--;
				if (retries == 0) throw TftpError(TftpError::ErrorType::Tftp, 0, "Max retries exceeded");

				block_num -= 1;
				ack_buffer[3] = recv_buffer[3];
				goto resend_ack_packet;
			}

			data.write(reinterpret_cast<char*>(recv_buffer + 4), recv_offset - 4);
			blksize_val = recv_offset - 4;
			block_num++;
			total_size += blksize_val;
		}
		break;
		case static_cast<uint8_t>(TftpOpcode::Error):
			throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), reinterpret_cast<char*>(recv_buffer + 4));
		default:
			throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
		}

		ack_buffer[2] = recv_buffer[2];
		ack_buffer[3] = recv_buffer[3];
	resend_ack_packet:
		if (sendto(sockfd, reinterpret_cast<char*>(ack_buffer), 4, 0, (struct sockaddr*)&comm_addr, comm_addr_len) == -1)
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send ack");

	} while (blksize_val == Config::BlockSize);

	// cleanup left to the guard
	return total_size;
}