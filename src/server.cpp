#include "../inc/tftp.hpp"

using namespace tftpc;

Server::Result Server::handleClient(socket_t sockfd, const std::string& root_dir) {
	ServerCleanupGuard guard;

	uint8_t* buffer = new uint8_t[Config::BlockSize + 4]();
	guard.guardNew(buffer);
	uint8_t* recv_buffer = new uint8_t[Config::BlockSize + 4]();
	guard.guardNew(recv_buffer);

	struct sockaddr_in client_addr = {};
	socklen_t client_addr_len = sizeof(client_addr);

	int recv_offset = -1;

	if ((recv_offset = recvfrom(sockfd, reinterpret_cast<char*>(recv_buffer), Config::BlockSize + 4, 0, (struct sockaddr*)&client_addr, &client_addr_len)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive request");

	if (recv_buffer[1] != static_cast<uint8_t>(TftpOpcode::ReadRequest) && recv_buffer[1] != static_cast<uint8_t>(TftpOpcode::WriteRequest))
		throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid request");

	Result result = { };

	uint16_t buffer_offset = 2;
	std::string filename = readStringFromBuffer(recv_buffer + 2, Config::BlockSize + 4 - buffer_offset);
	buffer_offset += static_cast<uint16_t>(filename.size() + 1);
	std::string mode = readStringFromBuffer(recv_buffer + filename.size() + 3, Config::BlockSize + 4 - buffer_offset);
	buffer_offset += static_cast<uint16_t>(mode.size() + 1);

	bool option_negotiation = false;
	size_t tsize = -1;
	uint16_t blksize = Config::BlockSize;
	uint16_t timeout = Config::Timeout;

	while (buffer_offset < recv_offset) {
		std::string option = readStringFromBuffer(recv_buffer + buffer_offset, Config::BlockSize + 4 - buffer_offset);
		buffer_offset += static_cast<uint16_t>(option.size() + 1);
		// next after null byte:
		std::string value_str = readStringFromBuffer(recv_buffer + buffer_offset, Config::BlockSize + 4 - buffer_offset);
		buffer_offset += static_cast<uint16_t>(value_str.size() + 1);

		uint32_t value = std::stoul(value_str);

		if (option == "tsize") tsize = value;
		else if (option == "blksize") blksize = static_cast<uint16_t>(value);
		else if (option == "timeout") timeout = static_cast<uint16_t>(value);

		option_negotiation = true;
	}

	result.type = recv_buffer[1] == static_cast<uint8_t>(TftpOpcode::ReadRequest) ? Result::Type::Read : Result::Type::Write;
	result.filename = filename;
	result.client_addr = client_addr;
	
	/* valid request create necessary variables */
	std::filesystem::path file_path = std::filesystem::path(root_dir) / filename;	// This is in std XDDD??? I hate overloading. Why not std::fs::concat_path(root_dir, filename);?
	socket_t comm_sockfd;
	struct sockaddr_in any_addr = {};
	any_addr.sin_family = AF_INET;
	any_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	any_addr.sin_port = htons(0);

	// create new socket for communication, as defined in RFC 1350. This server implementation doesn't really need it, it's here for compatibility.
	if ((comm_sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to create socket");
	guard.guardSocket(comm_sockfd);
	if (bind(comm_sockfd, (struct sockaddr*)&any_addr, sizeof(any_addr)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to bind socket");
#ifdef _WIN32
	DWORD sock_timeout = Config::Timeout * 1000;
#else
	struct timeval sock_timeout = { Config::Timeout, 0 };
#endif
	if (setsockopt(comm_sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&sock_timeout), sizeof(sock_timeout)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to set socket timeout");

	switch (recv_buffer[1]) {
	case static_cast<uint8_t>(TftpOpcode::ReadRequest): {
		std::ifstream file(file_path, std::ios::binary);
		if (!file.is_open()) {
			uint8_t err_buffer[512] = { 0, static_cast<uint8_t>(TftpOpcode::Error), 0, static_cast<uint8_t>(TftpErrorCode::FileNotFound) };
			std::string err_msg = "File not found";
			if (err_msg.size() > 512 - 5) err_msg.resize(512 - 5);
			strncpy_inc_offset(err_buffer, err_msg.c_str(), err_msg.size(), buffer_offset);

			if (sendto(comm_sockfd, reinterpret_cast<char*>(err_buffer), buffer_offset, 0, (struct sockaddr*)&client_addr, client_addr_len) == -1)
				throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send error response");

			throw TftpError(TftpError::ErrorType::IO, getOsError(), "Failed to open file");
		}
		guard.guardFile(std::move(file));

		if (option_negotiation) {
			buffer[1] = static_cast<uint8_t>(TftpOpcode::Oack);
			buffer_offset = 2;

			tsize = getStreamLength(file);
			std::string tsize_str = std::to_string(tsize);
			std::string blksize_str = std::to_string(blksize);
			std::string timeout_str = std::to_string(timeout);

			strncpy_inc_offset(buffer, "tsize", 5, buffer_offset);
			strncpy_inc_offset(buffer, tsize_str.c_str(), tsize_str.size(), buffer_offset);

			strncpy_inc_offset(buffer, "blksize", 7, buffer_offset);
			strncpy_inc_offset(buffer, blksize_str.c_str(), blksize_str.size(), buffer_offset);

			strncpy_inc_offset(buffer, "timeout", 8, buffer_offset);
			strncpy_inc_offset(buffer, timeout_str.c_str(), timeout_str.size(), buffer_offset);

			if (sendto(comm_sockfd, reinterpret_cast<char*>(buffer), buffer_offset, 0, (struct sockaddr*)&client_addr, client_addr_len) == -1)
				throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send OACK response");

			// 0-ack:
			if ((recv_offset = recvfrom(comm_sockfd, reinterpret_cast<char*>(recv_buffer), Config::BlockSize + 4, 0, (struct sockaddr*)&client_addr, &client_addr_len)) == -1)
				throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive ack");

			if (recv_buffer[1] != static_cast<uint8_t>(TftpOpcode::Ack))
				throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid ack opcode");

			if (recv_buffer[2] != 0 || recv_buffer[3] != 0)
				throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), "Invalid block number");
		}
		
		uint16_t block_num = 1;
		char data_header[4] = { 0, static_cast<uint8_t>(TftpOpcode::Data), 0, 0 };

		uint16_t retries = Config::MaxRetries;

#ifdef USE_PARALLEL_FILE_IO
		std::queue<std::unique_ptr<std::vector<uint8_t>>> data_queue;
		std::mutex data_queue_mutex;

		/* Chunk data into vectors with max. size of Config::BlockSize, except the last one */
		std::thread data_chunker([&file, &data_queue, &data_queue_mutex, blksize] {
			std::unique_ptr<std::vector<uint8_t>> data_chunk = std::make_unique<std::vector<uint8_t>>(blksize);
			while (file.read(reinterpret_cast<char*>(data_chunk->data()), blksize)) {
				while (data_queue.size() * blksize > Config::MaxQueueSize) continue;

				std::lock_guard<std::mutex> lock(data_queue_mutex);
				data_queue.push(std::move(data_chunk));
				data_chunk = std::make_unique<std::vector<uint8_t>>(blksize);
			}
			data_chunk->resize(file.gcount());
			std::lock_guard<std::mutex> lock(data_queue_mutex);
			data_queue.push(std::move(data_chunk));
			});

		guard.guardThread(std::move(data_chunker));

		while (data_queue.size() == 0) continue;
#endif

		bool size_divisible = (tsize % blksize == 0);

		do {
#ifdef USE_PARALLEL_FILE_IO
			std::unique_ptr<std::vector<uint8_t>> data_chunk = nullptr;
			{
				std::lock_guard<std::mutex> lock(data_queue_mutex);
				data_chunk = std::move(data_queue.front());
				data_queue.pop();
			}
#else
			std::unique_ptr<std::vector<uint8_t>> data_chunk = std::make_unique<std::vector<uint8_t>>(blksize);
			file.read(reinterpret_cast<char*>(data_chunk->data()), blksize);
			data_chunk->resize(file.gcount());
#endif
			data_header[2] = block_num >> 8;
			data_header[3] = block_num & 0xFF;

			buffer_offset = data_chunk->size();
			result.bytes_transferred += buffer_offset;

#ifdef _WIN32
			WSABUF packet[2];
			packet[0].buf = data_header;
			packet[0].len = 4;
			packet[1].buf = reinterpret_cast<char*>(buffer + 4);
			packet[1].len = buffer_offset;
#else
			struct iovec packet[2];
			packet[0].iov_base = data_header;
			packet[0].iov_len = 4;
			packet[1].iov_base = buffer + 4;
			packet[1].iov_len = buffer_offset;

			struct msghdr msg = {};
			msg.msg_name = &client_addr;
			msg.msg_namelen = client_addr_len;
			msg.msg_iov = packet;
			msg.msg_iovlen = 2;
#endif

		resend_data_packet:
#ifdef _WIN32
			DWORD bytes_sent;
			DWORD flags = 0;

			if (WSASendTo(comm_sockfd, packet, 2, &bytes_sent, flags, (struct sockaddr*)&client_addr, client_addr_len, nullptr, nullptr) == SOCKET_ERROR)
				throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send data");
#else
			if (sendmsg(comm_sockfd, &msg, 0) == -1)
				throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send data");
#endif

			// receive the client response (exp. ack)
			if ((recv_offset = recvfrom(comm_sockfd, reinterpret_cast<char*>(recv_buffer), Config::BlockSize + 4, 0, (struct sockaddr*)&client_addr, &client_addr_len)) == -1)
				throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive ack");

			if (recv_buffer[1] != static_cast<uint8_t>(TftpOpcode::Ack))
				throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid ack opcode");

			uint16_t block_num_ack = (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF);
			if (block_num_ack != block_num) {
				retries--;
				if (retries == 0) throw TftpError(TftpError::ErrorType::Tftp, 0, "Max retries exceeded");
				goto resend_data_packet;
			}
			

			block_num++;
			// XDDDDD Fix that
#ifdef USE_PARALLEL_FILE_IO
		} while (buffer_offset == blksize && data_queue.size() != 0);
#else
		} while (buffer_offset == blksize);
#endif

	} break;
	case static_cast<uint8_t>(TftpOpcode::WriteRequest): {

	} break;
	default:
		throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid request opcode");
	}

	guard.forceCleanup();
	return result;
}
