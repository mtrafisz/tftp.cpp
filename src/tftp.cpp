#include "../inc/tftp.hpp"

using namespace tftpc;

enum class TftpErrorCode : uint16_t {
	NotDefined = 0,
	FileNotFound = 1,
	AccessViolation = 2,
	DiskFull = 3,
	IllegalOperation = 4,
	UnknownTransferId = 5,
	FileAlreadyExists = 6,
	NoSuchUser = 7,
};

enum class TftpOpcode : uint16_t {
	ReadRequest = 1,
	WriteRequest = 2,
	Data = 3,
	Ack = 4,
	Error = 5,
	Oack = 6,
};

// u16 {a, b} -> u8 {b}
uint8_t getOpcodeByte(TftpOpcode opcode) {
	return static_cast<uint8_t>(static_cast<uint16_t>(opcode) & 0xFF);
}

void strncpy_inc_offset(uint8_t* buffer, const char* str, size_t len, uint16_t& offset) {
	std::copy(str, str + len, buffer + offset);
	offset += static_cast<uint16_t>(len);
	buffer[offset++] = '\0';
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

// safer reinterpret_cast<char*>
std::string readStringFromBuffer(uint8_t* buffer, size_t len) {
	auto null_byte = std::find(buffer, buffer + len, '\0');
	if (null_byte == buffer + len) {
		throw TftpError(TftpError::ErrorType::Tftp, 0, "Malformed packet");
	}

	return std::string(reinterpret_cast<char*>(buffer), null_byte - buffer);
}

void Client::send(const struct sockaddr_in& remote_addr, const std::string& filename, std::istream& data) {
	std::streamsize length = getStreamLength(data);		// todo - does this allways work?

	/* local address, socket and cleanup guard setup */
	struct sockaddr_in local_addr = {};
	local_addr.sin_family = AF_INET;
	local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
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

	CleanupGuard guard(sockfd);

	if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to bind socket");
#ifdef _WIN32
	DWORD timeout = Config::Timeout * 1000;
#else
	struct timeval timeout = { Config::Timeout, 0 };
#endif
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to set socket timeout");

	uint8_t* buffer = new uint8_t[Config::BlockSize]();
	uint8_t* recv_buffer = new uint8_t[Config::BlockSize]();

	guard.guardNew(buffer);
	guard.guardNew(recv_buffer);

	/* Create and send the request */
	uint16_t buffer_offset = 2;
	int32_t recv_offset = -1;
	buffer[1] = static_cast<uint8_t>(TftpOpcode::WriteRequest);

	uint16_t blksize_val = Config::BlockSize;
	std::string blksize_str = std::to_string(blksize_val);
	std::string tsize_str = std::to_string(length);
	std::string timeout_str = std::to_string(Config::Timeout);

	strncpy_inc_offset(buffer, filename.c_str(), filename.size(), buffer_offset);
	strncpy_inc_offset(buffer, "octet", 5, buffer_offset);

	strncpy_inc_offset(buffer, "tsize", 5, buffer_offset);
	strncpy_inc_offset(buffer, tsize_str.c_str(), tsize_str.size(), buffer_offset);

	strncpy_inc_offset(buffer, "blksize", 7, buffer_offset);
	strncpy_inc_offset(buffer, blksize_str.c_str(), blksize_str.size(), buffer_offset);

	strncpy_inc_offset(buffer, "timeout", 8, buffer_offset);
	strncpy_inc_offset(buffer, timeout_str.c_str(), timeout_str.size(), buffer_offset);

	if (sendto(sockfd, reinterpret_cast<char*>(buffer), buffer_offset, 0, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send request");

	/* Receive the server response and save new address of the server */
	struct sockaddr_in comm_addr = {};
	socklen_t comm_addr_len = sizeof(comm_addr);

	if ((recv_offset = recvfrom(sockfd, (char*)(recv_buffer), Config::BlockSize, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive response");
	if (recv_offset < 4) throw TftpError(TftpError::ErrorType::Tftp, 0, "Invalid response");

	/* Parse the response */
	switch (recv_buffer[1]) {
	case static_cast<uint8_t>(TftpOpcode::Oack):
		// TODO: check if params match
		break;
	case static_cast<uint8_t>(TftpOpcode::Ack):
		blksize_val = 512;
		break;
	case static_cast<uint8_t>(TftpOpcode::Error): {
		auto err_string = readStringFromBuffer(recv_buffer + 4, recv_offset - 4);
		throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), err_string);
	}
	default:
		throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
	}

	/* Data chunking and transfer */
	uint16_t block_num = 1;
	char data_header[4] = { 0, static_cast<uint8_t>(TftpOpcode::Data), 0, 0 };
#ifdef USE_PARALLEL_FILE_READ
	std::queue<std::unique_ptr<std::vector<uint8_t>>> data_queue;
	std::mutex data_queue_mutex;

	/* Chunk data into vectors with max. size of Config::BlockSize, except the last one */
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
#endif
	/* Data sending loop */

	// wait for single data chunk to be available (data_queue.front() will throw if empty)
#ifdef USE_PARALLEL_FILE_READ
	while (data_queue.size() == 0) continue;
#endif
	int retries = Config::MaxRetries;
	bool size_divisible = length % blksize_val == 0;

	do {
		// pop the front data chunk
	#ifdef USE_PARALLEL_FILE_READ
		std::unique_ptr<std::vector<uint8_t>> data_chunk = nullptr;
		{
			std::lock_guard<std::mutex> lock(data_queue_mutex);
			data_chunk = std::move(data_queue.front());
			data_queue.pop();
		}
	#else
		std::unique_ptr<std::vector<uint8_t>> data_chunk = std::make_unique<std::vector<uint8_t>>(blksize_val);
		data.read(reinterpret_cast<char*>(data_chunk->data()), blksize_val);
		data_chunk->resize(data.gcount());
	#endif

		// set the buffer offset to the size of the data chunk
		buffer_offset = static_cast<uint16_t>(data_chunk->size());	// this is one of conditions for the last chunk detection

		// create the data Message with multiple buffers (header + data)
		data_header[2] = block_num >> 8;
		data_header[3] = block_num & 0xFF;

#ifdef _WIN32
		WSABUF packet[2];
		packet[0].buf = data_header;
		packet[0].len = 4;
		packet[1].buf = reinterpret_cast<char*>(data_chunk->data());
		packet[1].len = buffer_offset;
#else
		struct iovec packet[2];
		packet[0].iov_base = data_header;
		packet[0].iov_len = 4;
		packet[1].iov_base = data_chunk->data();
		packet[1].iov_len = buffer_offset;

		struct msghdr msg = {};
		msg.msg_name = &comm_addr;
		msg.msg_namelen = comm_addr_len;
		msg.msg_iov = packet;
		msg.msg_iovlen = 2;
#endif
		// send the data Message
	resend_data_packet:
#ifdef _WIN32
		DWORD bytes_sent;
		DWORD flags = 0;

		if (WSASendTo(sockfd, packet, 2, &bytes_sent, flags, (struct sockaddr*)&comm_addr, comm_addr_len, nullptr, nullptr) == SOCKET_ERROR)
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send data");
#else
		if (sendmsg(sockfd, &msg, 0) == -1)
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send data");
#endif
		// receive the server response (exp. ack)
		if ((recv_offset = recvfrom(sockfd, reinterpret_cast<char*>(recv_buffer), Config::BlockSize, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 4) {
			auto errn = getOsError();

			switch (errn) {
			case TIMEOUT_OS_ERR:
				retries--;
				if (retries == 0) throw TftpError(TftpError::ErrorType::Tftp, 0, "Max retries exceeded");
				goto resend_data_packet;
			default:
				throw TftpError(TftpError::ErrorType::OS, errn, "Failed to receive response");
			}
		}

		// parse the server response
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
		case static_cast<uint8_t>(TftpOpcode::Error): {
			auto err_string = readStringFromBuffer(recv_buffer + 4, recv_offset - 4);
			throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), err_string);
		}
		default:
			throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
		}

// XDDDDD Fix that
#ifdef USE_PARALLEL_FILE_READ
	} while (buffer_offset == blksize_val && data_queue.size() != 0);
#else
	} while (buffer_offset == blksize_val);
#endif

	/* send empty data packet to signal end of transfer if there wasn't any data packet
	 * with size smaller than Config::BlockSize */
	if (size_divisible) {
		data_header[2] = block_num >> 8;
		data_header[3] = block_num & 0xFF;
		buffer_offset = 0;
	}

	guard.forceCleanup();
}

std::streamsize Client::recv(const struct sockaddr_in& remote_addr, const std::string& filename, std::ostream& data) {
	/* local address & socket setup */
	struct sockaddr_in local_addr = {};
	local_addr.sin_family = AF_INET;
	local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
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

	CleanupGuard guard(sockfd);

	if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to bind socket");
#ifdef _WIN32
	DWORD timeout = Config::Timeout * 1000;
#else
	struct timeval timeout = { Config::Timeout, 0 };
#endif
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to set socket timeout");


	/* Create and send the request */
	uint8_t* buffer = new uint8_t[Config::BlockSize + 4]();
	uint8_t* recv_buffer = new uint8_t[Config::BlockSize + 4]();

	guard.guardNew(buffer);
	guard.guardNew(recv_buffer);

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

	/* Receive the server response and save new address of the server */
	struct sockaddr_in comm_addr = {};
	socklen_t comm_addr_len = sizeof(comm_addr);

	if ((recv_offset = recvfrom(sockfd, (char*)(recv_buffer), Config::BlockSize, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 4)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive a valid response");

	/* Parse the response and send the ack */

	uint8_t ack_buffer[4] = { 0, static_cast<uint8_t>(TftpOpcode::Ack), 0, 0 };
	uint16_t block_num = 1;
	std::streamsize total_size = 0;

	switch (recv_buffer[1]) {
	case static_cast<uint8_t>(TftpOpcode::Oack):	// TODO: check if params match
		break;
	case static_cast<uint8_t>(TftpOpcode::Data): {	// negotiation broken, received first data packet
		uint16_t recv_blknum = (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF);
		if (recv_blknum != 1)
			throw TftpError(TftpError::ErrorType::Tftp, recv_blknum, "Invalid block number");

		blksize_val = recv_offset - 4;
		data.write(reinterpret_cast<char*>(recv_buffer + 4), blksize_val);
		ack_buffer[3] = recv_buffer[3];
		block_num++;
		total_size += blksize_val;
		break;
	}
	case static_cast<uint8_t>(TftpOpcode::Error):
		throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), reinterpret_cast<char*>(recv_buffer + 4));
	default:
		throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
	}

	if (sendto(sockfd, reinterpret_cast<char*>(ack_buffer), 4, 0, (struct sockaddr*)&comm_addr, comm_addr_len) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send ack");

	int retries = Config::MaxRetries;

	/* Data receiving loop */

	do {
		// receive the server response (exp. data)
		if ((recv_offset = recvfrom(sockfd, reinterpret_cast<char*>(recv_buffer), Config::BlockSize + 4, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 4) {
			auto errn = getOsError();

			switch (errn) {
			case TIMEOUT_OS_ERR:
				retries--;
				if (retries == 0) throw TftpError(TftpError::ErrorType::Tftp, 0, "Max retries exceeded");
				continue;
			default:
				throw TftpError(TftpError::ErrorType::OS, errn, "Failed to receive response");
			}
		}

		// parse the server response
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
			break;
		}
		case static_cast<uint8_t>(TftpOpcode::Error): {
			auto err_string = readStringFromBuffer(recv_buffer + 4, recv_offset - 4);
			throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), err_string);
		}
		default:
			throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
		}

		// create and send the ack packet
		ack_buffer[2] = recv_buffer[2];
		ack_buffer[3] = recv_buffer[3];
	resend_ack_packet:
		if (sendto(sockfd, reinterpret_cast<char*>(ack_buffer), 4, 0, (struct sockaddr*)&comm_addr, comm_addr_len) == -1)
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send ack");

	} while (blksize_val == Config::BlockSize);

	guard.forceCleanup();

	return total_size;
}

ServerResult Server::handleClient(socket_t sockfd, const std::string& root_dir) {
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

	ServerResult result = { };

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

#ifdef NEGOTIATE_OPTIONS
		option_negotiation = true;
#endif
	}

	result.type = recv_buffer[1] == static_cast<uint8_t>(TftpOpcode::ReadRequest) ? ServerResult::Type::Read : ServerResult::Type::Write;
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
		// concat filename with root_dir
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
		
		// TODO: the same thread trick as in Client::send

		uint16_t block_num = 1;
		char data_header[4] = { 0, static_cast<uint8_t>(TftpOpcode::Data), 0, 0 };

		uint16_t retries = Config::MaxRetries;

		do {
			file.read(reinterpret_cast<char*>(buffer + 4), blksize);
			std::streamsize read_size = file.gcount();
			result.bytes_transferred += read_size;

			data_header[2] = block_num >> 8;
			data_header[3] = block_num & 0xFF;

			if (read_size < blksize) {
				if (read_size == 0) break;
				buffer_offset = static_cast<uint16_t>(read_size);
			}
			else {
				buffer_offset = blksize;
			}

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
		} while (buffer_offset == blksize);

		file.close();

	} break;
	case static_cast<uint8_t>(TftpOpcode::WriteRequest): {

	} break;
	default:
		throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid request opcode");
	}

	guard.forceCleanup();
	return result;
}
