#include "../inc/tftp.hpp"

using namespace tftp;

void Client::send (
    const std::string& remote_addr_str,
    const std::string& filename,
    std::istream& data,
    ProgressCallback progress_callback,
    std::chrono::milliseconds callback_interval
) {
	Config config = Config::getInstance();

	std::streamsize length = getStreamLength(data);		// todo - does this allways work?

	/* local address, socket and cleanup guard setup */
	struct sockaddr_in local_addr = {};
    local_addr.sin_family = AF_INET;
	local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	local_addr.sin_port = htons(0);
	size_t local_addr_len = sizeof(local_addr);

    struct sockaddr_in remote_addr = {};
    remote_addr.sin_family = AF_INET;

    {
        std::string ip;
        std::string port;
        
        size_t pos = remote_addr_str.find(':');
        if (pos == std::string::npos) {
            ip = remote_addr_str;
            port = "69";
        } else {
            ip = remote_addr_str.substr(0, pos);
            port = remote_addr_str.substr(pos + 1);
        }

        inet_pton(AF_INET, ip.c_str(), &remote_addr.sin_addr);
        remote_addr.sin_port = htons(std::stoi(port));
    }

    if (remote_addr.sin_addr.s_addr == INADDR_NONE)
        throw TftpError(TftpError::ErrorType::OS, getOsError(), "Invalid IP address");

#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) 
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to initialize Winsock");
	
#endif

	socket_t sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to create socket");

	CleanupGuard guard(sockfd);

	if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to bind socket");
#ifdef _WIN32
	DWORD timeout = config.getTimeout() * 1000;
#else
	struct timeval timeout = { config.getTimeout(), 0 };
#endif
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == -1 ||
		setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to set socket timeout");

	uint8_t* buffer = new uint8_t[config.getBlockSize()]();
	uint8_t* recv_buffer = new uint8_t[config.getBlockSize()]();

	guard.guardNew(buffer);
	guard.guardNew(recv_buffer);

	/* Create and send the request */
	uint16_t buffer_offset = 2;
	int32_t recv_offset = -1;
	buffer[1] = static_cast<uint8_t>(TftpOpcode::WriteRequest);

	uint16_t blksize_val = config.getBlockSize();
	std::string blksize_str = std::to_string(blksize_val);
	std::string tsize_str = std::to_string(length);
	std::string timeout_str = std::to_string(config.getTimeout());

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

	if ((recv_offset = recvfrom(sockfd, (char*)(recv_buffer), config.getBlockSize(), 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) == -1)
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
		auto err_msg = readStringFromBuffer(recv_buffer + 4, recv_offset - 4);
		throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), err_msg);
	}
	default:
		throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
	}

	bool kill_child_threads = false;
    Progress progress_data(length);

	try {
	/* Progress callback thread */
	std::thread progress_thread;

	if (progress_callback) {
		progress_thread = std::thread ([&progress_callback, &progress_data, &callback_interval, &kill_child_threads] {
			while (progress_data.transferred_bytes < progress_data.total_bytes && !kill_child_threads) {
				std::this_thread::sleep_for(callback_interval);
				progress_callback(std::ref(progress_data));
			}
		});

		guard.guardThread(std::move(progress_thread));
	}

	/* Data chunking and transfer */
	uint16_t block_num = 1;
	char data_header[4] = { 0, static_cast<uint8_t>(TftpOpcode::Data), 0, 0 };
#ifdef USE_PARALLEL_FILE_IO
	std::queue<std::unique_ptr<std::vector<uint8_t>>> data_queue;
	std::mutex data_queue_mutex;
	size_t max_data_queue_size = config.getMaxQueueSize() / blksize_val;

	/* Chunk data into vectors with max. size of config.getBlockSize(), except the last one */
	std::thread data_chunker([&data, &data_queue, &data_queue_mutex, blksize_val, max_data_queue_size] {
		std::unique_ptr<std::vector<uint8_t>> data_chunk = std::make_unique<std::vector<uint8_t>>(blksize_val);
		while (data.read(reinterpret_cast<char*>(data_chunk->data()), blksize_val)) {
			while (data_queue.size() >= max_data_queue_size) continue;

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
#ifdef USE_PARALLEL_FILE_IO
	while (data_queue.size() == 0) continue;
#endif
	int retries = config.getMaxRetries();
	bool size_divisible = length % blksize_val == 0;

	do {
		// pop the front data chunk
	#ifdef USE_PARALLEL_FILE_IO
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
#else
		if (sendmsg(sockfd, &msg, 0) == -1)
#endif
		{
			auto errn = getOsError();

			switch (errn) {
			case TIMEOUT_OS_ERR:
				retries--;
				if (retries == 0) throw TftpError(TftpError::ErrorType::Tftp, 0, "Max retries exceeded");
				goto resend_data_packet;
			default:
				throw TftpError(TftpError::ErrorType::OS, errn, "Failed to send data");
			}
		}
		
		// receive the server response (exp. ack)
		if ((recv_offset = recvfrom(sockfd, reinterpret_cast<char*>(recv_buffer), config.getBlockSize(), 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 4) {
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

			progress_data.transferred_bytes += buffer_offset;

		} break;
		case static_cast<uint8_t>(TftpOpcode::Error): {
			auto err_msg = readStringFromBuffer(recv_buffer + 4, recv_offset - 4);
			throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), err_msg);
		}
		default:
			throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
		}

// XDDDDD Fix that
#ifdef USE_PARALLEL_FILE_IO
	} while (buffer_offset == blksize_val && data_queue.size() != 0);
#else
	} while (buffer_offset == blksize_val);
#endif

	/* send empty data packet to signal end of transfer if there wasn't any data packet
	 * with size smaller than config.getBlockSize() */
	if (size_divisible) {
		data_header[2] = block_num >> 8;
		data_header[3] = block_num & 0xFF;

		if (sendto(sockfd, reinterpret_cast<char*>(data_header), 4, 0, (struct sockaddr*)&comm_addr, comm_addr_len) == -1)
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send data");
	}
	} catch (TftpError& e) {
		kill_child_threads = true;
		throw e;
	} catch (std::exception& e) {
		kill_child_threads = true;
		throw TftpError(TftpError::ErrorType::Lib, 0, e.what());
	} catch (...) {
		kill_child_threads = true;
		throw TftpError(TftpError::ErrorType::OS, 0, "Unknown error");
	}

	if (progress_callback) progress_callback(progress_data);

	guard.forceCleanup();
}

std::streamsize Client::recv (
    const std::string& remote_addr_str,
    const std::string& filename,
    std::ostream& data,
    ProgressCallback progress_callback,
    std::chrono::milliseconds callback_interval
) {
	// get library config
	Config& config = Config::getInstance();

	/* local address & socket setup */
	struct sockaddr_in local_addr = {};
    local_addr.sin_family = AF_INET;
	local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	local_addr.sin_port = htons(0);
	size_t local_addr_len = sizeof(local_addr);

    struct sockaddr_in remote_addr = {};
    remote_addr.sin_family = AF_INET;

    {
        std::string ip;
        std::string port;
        
        size_t pos = remote_addr_str.find(':');
        if (pos == std::string::npos) {
            ip = remote_addr_str;
            port = "69";
        } else {
            ip = remote_addr_str.substr(0, pos);
            port = remote_addr_str.substr(pos + 1);
        }

        inet_pton(AF_INET, ip.c_str(), &remote_addr.sin_addr);
        remote_addr.sin_port = htons(std::stoi(port));
    }

    if (remote_addr.sin_addr.s_addr == INADDR_NONE)
        throw TftpError(TftpError::ErrorType::OS, getOsError(), "Invalid IP address");

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
	DWORD timeout = config.getTimeout() * 1000;
#else
	struct timeval timeout = { config.getTimeout(), 0 };
#endif
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == -1 || 
		setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to set socket timeout");

	/* Create and send the request */
	uint8_t* buffer = new uint8_t[config.getBlockSize() + 4]();
	uint8_t* recv_buffer = new uint8_t[config.getBlockSize() + 4]();

	guard.guardNew(buffer);
	guard.guardNew(recv_buffer);

	uint16_t buffer_offset = 2;
	int32_t recv_offset = -1;
	buffer[1] = static_cast<uint8_t>(TftpOpcode::ReadRequest);

	uint16_t blksize_val = config.getBlockSize();
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

	if ((recv_offset = recvfrom(sockfd, (char*)(recv_buffer), config.getBlockSize(), 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 4)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive a valid response");

	/* Parse the response and send the ack */

	uint8_t ack_buffer[4] = { 0, static_cast<uint8_t>(TftpOpcode::Ack), 0, 0 };
	uint16_t block_num = 1;
	std::streamsize total_size = 0;
	std::streamsize expected_size = 0;

	switch (recv_buffer[1]) {
	case static_cast<uint8_t>(TftpOpcode::Oack): {
		// read the options - save the tsize

		size_t offset = 2;
		while (offset < recv_offset) {
			std::string option = readStringFromBuffer(recv_buffer + offset, recv_offset - offset);
			offset += option.size() + 1;
			std::string value = readStringFromBuffer(recv_buffer + offset, recv_offset - offset);
			offset += value.size() + 1;

			if (option == "tsize") {
				expected_size = std::stoll(value);
			}
			else if (option == "blksize") {
				uint16_t blksize_ack_val = std::stoi(value);

				if (blksize_ack_val > blksize_val) throw TftpError(TftpError::ErrorType::Tftp, 0, "Invalid block size");
				blksize_val = blksize_ack_val;
			}
		}

	break;
	}
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
	case static_cast<uint8_t>(TftpOpcode::Error): {
		auto err_msg = readStringFromBuffer(recv_buffer + 4, recv_offset - 4);
		throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), err_msg);
	}
	default:
		throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
	}

	if (sendto(sockfd, reinterpret_cast<char*>(ack_buffer), 4, 0, (struct sockaddr*)&comm_addr, comm_addr_len) == -1)
		throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send ack");

	/* Data receiving loop */

	bool kill_child_threads = false;
	Progress progress_data(expected_size);
	try {
	// Progress callback thread
	std::thread progress_thread;

	if (progress_callback) {
		progress_thread = std::thread([&progress_callback, &progress_data, &callback_interval, &kill_child_threads] {
			while (progress_data.transferred_bytes < progress_data.total_bytes && !kill_child_threads) {
				std::this_thread::sleep_for(callback_interval);
				progress_callback(std::ref(progress_data));
			}
		});

		guard.guardThread(std::move(progress_thread));
	}

#ifdef USE_PARALLEL_FILE_IO
    std::queue<std::unique_ptr<std::vector<uint8_t>>> data_queue;
    std::mutex data_queue_mutex;
	bool transfer_done = false;

	// data writer thread
	std::thread data_writer([&data, &data_queue, &data_queue_mutex, &transfer_done] {
		std::this_thread::sleep_for(std::chrono::seconds(2));

		while (true) {
			if (data_queue.size() == 0) {	// transfer errored-out or finished
				if (transfer_done) break;
				continue;
			}
			std::unique_ptr<std::vector<uint8_t>> data_chunk = nullptr;
			{
				std::lock_guard<std::mutex> lock(data_queue_mutex);
				data_chunk = std::move(data_queue.front());
				data_queue.pop();
			}
			data.write(reinterpret_cast<char*>(data_chunk->data()), data_chunk->size());
		}
	});
	guard.guardThread(std::move(data_writer));
#endif

	do {
		// receive the server response (exp. data)
		if ((recv_offset = recvfrom(sockfd, reinterpret_cast<char*>(recv_buffer), config.getBlockSize() + 4, 0, (struct sockaddr*)&comm_addr, &comm_addr_len)) < 4) {
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to receive response");
		}

		// parse the server response
		switch (recv_buffer[1]) {
		case static_cast<uint8_t>(TftpOpcode::Data): {
			uint16_t recv_blknum = (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF);
			if (recv_blknum != block_num) {
				throw TftpError(TftpError::ErrorType::Tftp, recv_blknum, "Invalid block number");
			}
			break;
		}
		case static_cast<uint8_t>(TftpOpcode::Error): {
			auto err_msg = readStringFromBuffer(recv_buffer + 4, recv_offset - 4);
			throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), err_msg);
		}
		default:
			throw TftpError(TftpError::ErrorType::Tftp, recv_buffer[1], "Invalid response opcode");
		}
		#ifndef USE_PARALLEL_FILE_IO
        	data.write(reinterpret_cast<char*>(recv_buffer + 4), recv_offset - 4);
		#else
		{
			if (data_queue.size() > (size_t)config.getMaxQueueSize() / blksize_val) continue;

			std::unique_ptr<std::vector<uint8_t>> data_chunk = std::make_unique<std::vector<uint8_t>>(recv_offset - 4);
			std::copy(recv_buffer + 4, recv_buffer + recv_offset, data_chunk->data());
			std::lock_guard<std::mutex> lock(data_queue_mutex);
			data_queue.push(std::move(data_chunk));
		}
		#endif
        blksize_val = recv_offset - 4;
        block_num++;
        total_size += blksize_val;
		
		progress_data.transferred_bytes += blksize_val;

		// create and send the ack packet
		ack_buffer[2] = recv_buffer[2];
		ack_buffer[3] = recv_buffer[3];
		if (sendto(sockfd, reinterpret_cast<char*>(ack_buffer), 4, 0, (struct sockaddr*)&comm_addr, comm_addr_len) == -1) {
			throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to send ack");
		}
	} while (blksize_val == config.getBlockSize());
#ifdef USE_PARALLEL_FILE_IO
	transfer_done = true;
#endif
	} catch (TftpError& e) {
		kill_child_threads = true;
		throw e;
	} catch (std::exception& e) {
		kill_child_threads = true;
		throw TftpError(TftpError::ErrorType::Lib, 0, e.what());
	} catch (...) {
		kill_child_threads = true;
		throw TftpError(TftpError::ErrorType::OS, 0, "Unknown error");
	}

	if (progress_callback) progress_callback(progress_data);

	guard.forceCleanup();

	return total_size;
}
