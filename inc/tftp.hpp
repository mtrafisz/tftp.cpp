#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <stdexcept>
#include <string>
#include <vector>
#include <ostream>
#include <memory>
#include <istream>      // for std::istream
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <queue>

namespace tftpc {

    /* Things You can edit, to change how library works: */
    struct Config {
		static constexpr uint16_t BlockSize = 16e+3;     // smaller -> better for smaller files and bad connections but VERY SLOW
		// ^ TODO thing crashes with 32k block size XD
        static constexpr uint16_t Timeout = 5;
        static constexpr uint16_t MaxRetries = 5;
    };
    // thats it :)

    class TftpError : public std::runtime_error {
    public:
        enum class ErrorType {
            None,
            Lib,
            Tftp,
            IO,
            OS,
            Timeout,
        };

        TftpError(ErrorType type, int code, const std::string& msg)
            : std::runtime_error(msg), type_(type), code_(code) {}

        ErrorType getType() const { return type_; }
        int getCode() const { return code_; }

        // nicer printing:
        friend std::ostream& operator<<(std::ostream& os, const TftpError& err) {
            os << "TFTP error <" << err.code_ << ">: " << err.what();
            return os;
        }

    private:
        ErrorType type_;
        int code_;
    };

    class Client {
    public:
        /// <summary>
		/// Attempts to read data from stream and send it to the server via TFTP.
        /// </summary>
        /// <param name="remote_addr">IPv4 address of server.</param>
        /// <param name="filename">How the file will be saved on server.</param>
        /// <param name="data">Stream containing desired file contents.</param>
        static void send(const struct sockaddr_in& remote_addr, const std::string& filename, std::istream& data);
        /// <summary>
		/// Attempts to read data from server via TFTP and write it to stream.
        /// </summary>
		/// <param name="remote_addr">IPv4 address of server.</param>
		/// <param name="filename">Name of the file on server.</param>
		/// <param name="data">Stream to write file contents to.</param>'
		/// <returns>Number of bytes received.</returns>
        static std::streamsize recv(const struct sockaddr_in& remote_addr, const std::string& filename, std::ostream& data);
    
    private:
        class CleanupGuard {
        public:
            CleanupGuard(SOCKET sockfd) : sockfd_(sockfd), needs_cleanup_(true) {}
            ~CleanupGuard() {
                if (needs_cleanup_) {
					for (auto& t : threads_) {
						t.join();
					}

                    #ifdef _WIN32
                        closesocket(sockfd_);
                        WSACleanup();
                    #else
                        close(sockfd);
                    #endif
                }
            }
            void dismiss() { needs_cleanup_ = false; }
			void guardThread(std::thread&& t) {
				threads_.push_back(std::move(t));
			}

        private:
            SOCKET sockfd_;
			std::vector<std::thread> threads_;
            bool needs_cleanup_;
        };
    };

    namespace server {
        void handleClient(int sockfd, const std::string& root_dir);
    }
    
    namespace {
        #ifdef _WIN32
            typedef int socklen_t;
            typedef SOCKET socket_t;
        #else 
            typedef int socket_t;
        #endif

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

        void VecPushString(std::vector<uint8_t>& packet, size_t& packet_offset, const char* str) {
            packet.insert(packet.begin() + packet_offset, str, str + strlen(str));
            packet.push_back(0);
            packet_offset += strlen(str) + 1;
        }

        void VecPushString(std::vector<uint8_t>& packet, size_t& packet_offset, const std::string& str) {
            packet.insert(packet.begin() + packet_offset, str.begin(), str.end());
            packet.push_back(0);
            packet_offset += str.size() + 1;
        }

        struct Constants {
            // nice to have when copying memory
            static constexpr char* modeOctet = "octet";
            static constexpr size_t modeOctetLen = 5;
            static constexpr char* optTsize = "tsize";
            static constexpr size_t optTsizeLen = 5;
            static constexpr char* optTimeout = "timeout";
            static constexpr size_t optTimeoutLen = 7;
            static constexpr char* optBlksize = "blksize";
            static constexpr size_t optBlksizeLen = 7;
        };
    }
}




