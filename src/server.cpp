#include "../inc/tftp.hpp"

using namespace tftp;

bool checkFileWriteable(const std::filesystem::path& file_path) {
    if (std::filesystem::exists(file_path)) {
        if (!std::filesystem::is_regular_file(file_path)) {
            return false;
        }
    }

    std::ofstream test_file;
    try {
        test_file = std::ofstream(file_path);
        if (!test_file.is_open()) {
            return false;
        }
    } catch (const std::exception&) {
        return false;
    }
    
    return true;
}

bool checkFileReadable(const std::filesystem::path& file_path) {
    if (!std::filesystem::exists(file_path) || !std::filesystem::is_regular_file(file_path)) {
        return false;
    }

    std::ifstream test_file;
    try {
        test_file = std::ifstream(file_path);
        if (!test_file.is_open()) {
            return false;
        }
    } catch (const std::exception&) {
        return false;
    }

    return true;
}

void sendErrorPacket(socket_t sockfd, const struct sockaddr_in& client_addr, TftpError::ErrorCode error_code, const std::string& error_msg) {
    uint8_t* buffer = new uint8_t[error_msg.size() + 5]();
    buffer[0] = 0;
    buffer[1] = static_cast<uint8_t>(TftpOpcode::Error);
    buffer[2] = 0;
    buffer[3] = static_cast<uint8_t>(error_code);
    std::copy(error_msg.begin(), error_msg.end(), buffer + 4);
    buffer[error_msg.size() + 4] = '\0';

    if (sendto(sockfd, reinterpret_cast<char*>(buffer), error_msg.size() + 5, 0, (struct sockaddr*)&client_addr, sizeof(client_addr)) < 0)
        throw std::runtime_error("Failed to send error packet to client");

    delete[] buffer;
}

void Server::handleClient (
    socket_t sockfd,
    const std::string& root_dir,
    TransferCallback callback,
    std::chrono::milliseconds callback_interval
){
    Config config = Config::getInstance();
    ServerCleanupGuard guard;

    uint8_t* buffer = new uint8_t[config.getBlockSize() + 4]();
    uint8_t* recv_buffer = new uint8_t[config.getBlockSize() + 4]();
    guard.guardNew(buffer);
    guard.guardNew(recv_buffer);

    struct sockaddr_in client_addr = {};
    socklen_t client_addr_len = sizeof(client_addr);

    int recv_offset = -1;

    if ((recv_offset = recvfrom(sockfd, reinterpret_cast<char*>(recv_buffer), config.getBlockSize() + 4, 0, (struct sockaddr*)&client_addr, &client_addr_len)) < 0)
        return;
    
    if (recv_buffer[1] != static_cast<uint8_t>(TftpOpcode::ReadRequest) && recv_buffer[1] != static_cast<uint8_t>(TftpOpcode::WriteRequest)) {
        sendErrorPacket(sockfd, client_addr, TftpError::ErrorCode::IllegalOperation, "Illegal TFTP operation");
        return;
    }

    TransferInfo info;

    uint16_t buffer_offset = 2;
    std::string request_filename = readStringFromBuffer(recv_buffer + 2, config.getBlockSize() + 4 - buffer_offset);
    buffer_offset += request_filename.size() + 1;
    std::string mode = readStringFromBuffer(recv_buffer + buffer_offset, config.getBlockSize() + 4 - buffer_offset);
    buffer_offset += mode.size() + 1;

    bool option_negotiation = false;
    size_t tsize = 0;
    uint16_t blksize = config.getBlockSize();   // is treated as max block size
    uint16_t timeout = config.getTimeout();

    while (buffer_offset < recv_offset) {
        option_negotiation = true;

        std::string option = readStringFromBuffer(recv_buffer + buffer_offset, config.getBlockSize() + 4 - buffer_offset);
        buffer_offset += option.size() + 1;
        std::string value = readStringFromBuffer(recv_buffer + buffer_offset, config.getBlockSize() + 4 - buffer_offset);
        buffer_offset += value.size() + 1;

        uint32_t value_int = std::stoul(value);

        if (option == "tsize") tsize = value_int;
        else if (option == "blksize") blksize = static_cast<uint16_t>(value_int);
        else if (option == "timeout") timeout = static_cast<uint16_t>(value_int);
    }

    if (blksize > config.getBlockSize()) {
        blksize = config.getBlockSize();
    }

    info.type = (recv_buffer[1] == static_cast<uint8_t>(TftpOpcode::ReadRequest)) ? TransferInfo::Type::Read : TransferInfo::Type::Write;
    info.client_addr = client_addr;
    info.filename = request_filename;
    info.total_bytes = tsize;
    info.transferred_bytes = 0;

    std::filesystem::path file_path = std::filesystem::path(root_dir) / request_filename;
    socket_t comm_sockfd;

    struct sockaddr_in comm_addr = {};
    comm_addr.sin_family = AF_INET;
    comm_addr.sin_port = 0;
    comm_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if ((comm_sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        throw std::runtime_error("Failed to create communication socket");
    guard.guardSocket(comm_sockfd);
    if (bind(comm_sockfd, (struct sockaddr*)&comm_addr, sizeof(comm_addr)) < 0)
        throw std::runtime_error("Failed to bind communication socket");

#ifdef _WIN32
    DWORD timeout_val = timeout * 1000;
#else
    struct timeval timeout_val = {timeout, 0};

#endif
    if (setsockopt(comm_sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_val), sizeof(timeout_val)) < 0)
        throw TftpError(TftpError::ErrorType::OS, getOsError(), "Failed to set socket timeout");

    if (info.type == TransferInfo::Type::Read && !checkFileReadable(file_path)) {
        sendErrorPacket(comm_sockfd, info.client_addr, TftpError::ErrorCode::FileNotFound, "File not found");
        return;
    } else if (info.type == TransferInfo::Type::Write && !checkFileWriteable(file_path)) {
        sendErrorPacket(comm_sockfd, info.client_addr, TftpError::ErrorCode::AccessViolation, "Access violation");
        return;
    }

    
    if (option_negotiation) {
        buffer[1] = static_cast<uint8_t>(TftpOpcode::Oack);
        buffer_offset = 2;

        std::string blksize_str = std::to_string(blksize);
        std::string timeout_str = std::to_string(timeout);
        
        if (info.type == TransferInfo::Type::Read) {
            info.total_bytes = getStreamLength(std::ifstream(file_path));
        }

        std::string tsize_str = std::to_string(info.total_bytes);

        strncpy_inc_offset(buffer, "blksize", 7, buffer_offset);
        strncpy_inc_offset(buffer, blksize_str.c_str(), blksize_str.size(), buffer_offset);
        strncpy_inc_offset(buffer, "timeout", 7, buffer_offset);
        strncpy_inc_offset(buffer, timeout_str.c_str(), timeout_str.size(), buffer_offset);
        strncpy_inc_offset(buffer, "tsize", 5, buffer_offset);
        strncpy_inc_offset(buffer, tsize_str.c_str(), tsize_str.size(), buffer_offset);

        // send oack
        if (sendto(comm_sockfd, reinterpret_cast<char*>(buffer), buffer_offset, 0, (struct sockaddr*)&client_addr, sizeof(client_addr)) < 0)
            throw std::runtime_error("Failed to send OACK packet to client");
        // recv 0 ack
        if ((recv_offset = recvfrom(comm_sockfd, reinterpret_cast<char*>(recv_buffer), config.getBlockSize() + 4, 0, (struct sockaddr*)&client_addr, &client_addr_len)) < 0)
            throw std::runtime_error("Failed to receive data from client");

        if (recv_buffer[1] != static_cast<uint8_t>(TftpOpcode::Ack)) {
            sendErrorPacket(comm_sockfd, client_addr, TftpError::ErrorCode::IllegalOperation, "Illegal TFTP operation");
            return;
        } else {
            buffer_offset = 2;
            uint16_t block_num = ntohs(*reinterpret_cast<uint16_t*>(recv_buffer + 2));
            if (block_num != 0) {
                sendErrorPacket(comm_sockfd, client_addr, TftpError::ErrorCode::IllegalOperation, "Illegal TFTP operation");
                return;
            }
        }
    } // else we just send data

    // callback thread:
    if (callback) {
        std::thread callback_thread = std::thread([callback, &info, callback_interval] {
            while (info.transferred_bytes < info.total_bytes) {
                std::this_thread::sleep_for(callback_interval);
                callback(info);
            }
        });
        guard.guardThread(std::move(callback_thread));
    }
    
    if (info.type == TransferInfo::Type::Read) {
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            sendErrorPacket(comm_sockfd, info.client_addr, TftpError::ErrorCode::FileNotFound, "File not found");
            return;
        }

        uint16_t block_num = 1;
        char data_header[4] = {0, static_cast<char>(TftpOpcode::Data), 0, 0};
    #ifdef USE_PARALLEL_FILE_IO
        std::queue<std::unique_ptr<std::vector<uint8_t>>> data_queue;
        std::mutex data_queue_mutex;
        size_t max_queue_size = config.getMaxQueueSize() / blksize;

        std::thread data_chunker([&file, &data_queue, &data_queue_mutex, blksize, max_queue_size] {
            std::unique_ptr<std::vector<uint8_t>> data_chunk = std::make_unique<std::vector<uint8_t>>(blksize + 4);

            while (file.read(reinterpret_cast<char*>(data_chunk->data() + 4), blksize)) {
                while (data_queue.size() >= max_queue_size) continue;

                {std::lock_guard<std::mutex> lock(data_queue_mutex);
                data_queue.push(std::move(data_chunk));}
                data_chunk = std::make_unique<std::vector<uint8_t>>(blksize + 4);
            }

            data_chunk->resize(file.gcount() + 4);
            std::lock_guard<std::mutex> lock(data_queue_mutex);
            data_queue.push(std::move(data_chunk));
        });

        guard.guardThread(std::move(data_chunker));

        while (data_queue.size() == 0) continue;
    #endif

        int retries = config.getMaxRetries();
        bool size_divisible = info.total_bytes % blksize == 0;

        do {
        #ifdef USE_PARALLEL_FILE_IO
            std::unique_ptr<std::vector<uint8_t>> data_chunk = nullptr;
            {
                std::lock_guard<std::mutex> lock(data_queue_mutex);
                data_chunk = std::move(data_queue.front());
                data_queue.pop();
            }
        #else
            std::unique_ptr<std::vector<uint8_t>> data_chunk = std::make_unique<std::vector<uint8_t>>(blksize_val);
            file.read(reinterpret_cast<char*>(data_chunk->data()), blksize_val);
            data_chunk->resize(file.gcount());
        #endif
            
            buffer_offset = static_cast<uint16_t>(data_chunk->size());

            data_header[2] = block_num >> 8;
            data_header[3] = block_num & 0xFF;

        #ifdef _WIN32
            WSABUF packet[2];
            packet[0].buf = data_header;    
            packet[0].len = 4;
            packet[1].buf = reinterpret_cast<char*>(data_chunk->data());
            packet[1].len = buffer_offset;

            DWORD bytes_sent;
            DWORD flags = 0;

            if (WSASendTo(comm_sockfd, packet, 2, &bytes_sent, flags, (struct sockaddr*)&client_addr, sizeof(client_addr), nullptr, nullptr) == SOCKET_ERROR)
                throw std::runtime_error("Failed to send data packet to client");
        #else
            struct iovec packet[2];
            packet[0].iov_base = data_header;
            packet[0].iov_len = 4;
            packet[1].iov_base = reinterpret_cast<char*>(data_chunk->data());
            packet[1].iov_len = buffer_offset;

            struct msghdr msg = {};
            msg.msg_name = &client_addr;
            msg.msg_namelen = sizeof(client_addr);
            msg.msg_iov = packet;
            msg.msg_iovlen = 2;

            if (sendmsg(comm_sockfd, &msg, 0) < 0)
                throw std::runtime_error("Failed to send data packet to client");
        #endif

            if ((recv_offset = recvfrom(comm_sockfd, reinterpret_cast<char*>(recv_buffer), config.getBlockSize() + 4, 0, (struct sockaddr*)&client_addr, &client_addr_len)) < 0) {
                auto errnum = getOsError();

                if (errnum == TIMEOUT_OS_ERR) {
                    if (retries == 0) {
                        sendErrorPacket(comm_sockfd, client_addr, TftpError::ErrorCode::UnknownTransferId, "Transfer ID unknown");
                        return;
                    }
                    retries--;
                    continue;
                } else {
                    throw std::runtime_error("Failed to receive data from client");
                }
            }

            switch (static_cast<TftpOpcode>(recv_buffer[1])) {
                case TftpOpcode::Ack: {
                    uint16_t recv_block_num = ntohs(*reinterpret_cast<uint16_t*>(recv_buffer + 2));
                    if (recv_block_num != block_num) {
                        sendErrorPacket(comm_sockfd, client_addr, TftpError::ErrorCode::UnknownTransferId, "Transfer ID unknown");
                        return;
                    }
                    block_num++;
                    retries = config.getMaxRetries();
                    break;
                }
                case TftpOpcode::Error: {
                    std::string error_msg = readStringFromBuffer(recv_buffer + 4, config.getBlockSize() + 4 - 4);
                    throw TftpError(TftpError::ErrorType::Tftp, (recv_buffer[2] << 8) | (recv_buffer[3] & 0xFF), error_msg);
                }
                default:
                    sendErrorPacket(comm_sockfd, client_addr, TftpError::ErrorCode::IllegalOperation, "Illegal TFTP operation");
                    return;
            }

            info.transferred_bytes += buffer_offset;
    #ifdef USE_PARALLEL_FILE_IO
        } while (buffer_offset == blksize && data_queue.size() != 0);
    #else
        } while (buffer_offset == blksize);
    #endif

        if (size_divisible) {
            data_header[2] = block_num >> 8;
            data_header[3] = block_num & 0xFF;

            if (sendto(comm_sockfd, reinterpret_cast<char*>(data_header), 4, 0, (struct sockaddr*)&client_addr, sizeof(client_addr)) < 0)
                throw std::runtime_error("Failed to send last data packet to client");
        }

        callback(info);
        guard.forceCleanup();
        return;
    }
    else if (info.type == TransferInfo::Type::Write) {
        // transfer loop
    }
    else {
        throw std::runtime_error("Invalid transfer type");
    }

    
}
