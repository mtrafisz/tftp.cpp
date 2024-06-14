#pragma once

#ifdef _WIN32
#include <WS2tcpip.h>
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
#include <istream>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <queue>
#include <fstream>
#include <filesystem>
#include <functional>
#include <chrono>

namespace tftp {
    /* Things You can edit, to change how library works: */

    // if defined, will use parallel file read, which is faster but uses more memory (up to file size at once at peak)
	// in other case, data will be read from file as needed (one chunk at the time) - transfer will be limited by disk read speed
    // WARNING - this adds some overhead to every DATA - ACK transaction, if you're using blksize smaller than 2048, undef it.
    #define USE_PARALLEL_FILE_IO

    struct Config {
		// TODO thing crashes with 32k block size XD
		static constexpr uint16_t BlockSize = 8192;     // smaller -> better for smaller files and bad connections but transfers slow down considerably
        static constexpr uint16_t Timeout = 5;
        static constexpr uint16_t MaxRetries = 5;
        static constexpr std::streamsize MaxQueueSize = 100 * (1 << 20);    // 100 MB by default, max memory usage will be this + around 10%, idk why.
                                                                            // Only enforced in the Client::send
    };
    // thats it :)

#ifdef _WIN32
    typedef SOCKET socket_t;
	typedef int socklen_t;
#define clean_sockfd(sockfd) do { closesocket(sockfd); WSACleanup(); } while(0)
#define getOsError() WSAGetLastError()
#define TIMEOUT_OS_ERR WSAETIMEDOUT
#else
	typedef int socket_t;
#define clean_sockfd(sockfd) do { close(sockfd); } while(0)
#define getOsError() errno
#define TIMEOUT_OS_ERR ETIMEDOUT
#endif

    /// <summary>
	/// Error type thrown by TFTP functions.
    /// </summary>
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

        friend std::ostream& operator<<(std::ostream& os, const TftpError& err) {
            switch (err.type_) {
			case ErrorType::None: os << "No error"; break;
            case ErrorType::Lib: os << "Library error"; break;
			case ErrorType::Tftp: os << "TFTP error"; break;
			case ErrorType::IO: os << "IO error"; break;
			case ErrorType::OS: os << "OS error"; break;
			case ErrorType::Timeout: os << "Timeout"; break;
			default: os << "Unknown error"; break;
            }
            if (err.type_ != ErrorType::None) {
                os << " <code: " << err.code_ << ">: " << err.what();
            }
            return os;
        }

    private:
        ErrorType type_;
        int code_;
    };

    class Client {
    public:
        class Progress {
        public:
            size_t total_bytes;
            size_t transferred_bytes;
            bool transfer_active() const { return transferred_bytes < total_bytes; }

            Progress(size_t total_bytes) : total_bytes(total_bytes), transferred_bytes(0) {}
        };

        typedef std::function<void(Progress&)> ProgressCallback;

        static void send (
            const std::string& remote_addr,
            const std::string& filename,
            std::istream& data,
            ProgressCallback progress = nullptr,
            std::chrono::milliseconds callback_interval = std::chrono::milliseconds(1000)
        );

        static std::streamsize recv (
            const std::string& remote_addr,
            const std::string& filename,
            std::ostream& data,
            ProgressCallback progress = nullptr,
            std::chrono::milliseconds callback_interval = std::chrono::milliseconds(1000)
        );
    
    private:
        class CleanupGuard {
        public:
            CleanupGuard(socket_t sockfd) : sockfd_(sockfd), needs_cleanup_(true) {}
            ~CleanupGuard() {
				cleanup();
            }
			void forceCleanup() { needs_cleanup_ = true; cleanup(); }
            void dismiss() { needs_cleanup_ = false; }

			void guardThread(std::thread&& t) {
				threads_.push_back(std::move(t));
			}
			template <typename T>
            void guardNew(T* ptr) {
				news_.push_back(static_cast<void*>(ptr));
            }

        private:
            socket_t sockfd_;
			std::vector<std::thread> threads_;
            std::vector<void*> news_;
            bool needs_cleanup_;

			void cleanup() {
				if (needs_cleanup_) {
                    if (needs_cleanup_) {
                        for (auto& t : threads_) {
                            t.join();
                        }
                        for (auto& ptr : news_) {
                            delete[] ptr;
                        }

                        clean_sockfd(sockfd_);
						needs_cleanup_ = false;
                    }
				}
			}
        };
    };

    class Server {
    public:
        class Result {
        public:
            enum class Type {
                None,
                Read,
                Write,
            };

            Type type;
            struct sockaddr_in client_addr;
            std::string filename;
            std::streamsize bytes_transferred;

            // nice printing:
            friend std::ostream& operator<<(std::ostream& os, const Result& result) {
                // address:port read from/wrote to filename (bytes)
                os << inet_ntop(AF_INET, &result.client_addr.sin_addr, nullptr, 0) << ":" << ntohs(result.client_addr.sin_port) << " ";
                switch (result.type) {
                case Type::Read: os << "read from "; break;
                case Type::Write: os << "wrote to "; break;
                default: os << "unknown action on "; break;
                }
                os << result.filename << " (" << result.bytes_transferred << " bytes)";
                return os;
            }
        };
        
        static Result handleClient(socket_t sockfd, const std::string& root_dir);

	private:
        class ServerCleanupGuard {
        public:
            ServerCleanupGuard() : needs_cleanup_(true) {}
            ~ServerCleanupGuard() {
                cleanup();
            }
            void forceCleanup() { needs_cleanup_ = true; cleanup(); }
            void dismiss() { needs_cleanup_ = false; }

            template <typename T>
            void guardNew(T* ptr) {
                news_.push_back(static_cast<void*>(ptr));
            }

			void guardSocket(socket_t sockfd) {
				sockfd_ = sockfd;
			}

            void guardThread(std::thread&& t) {
                threads_.push_back(std::move(t));
            }

			void guardFile(std::ifstream&& file) {
				file_ = std::move(file);
			}

        private:
            socket_t sockfd_;
            bool needs_cleanup_;
			std::ifstream file_;
            std::vector<void*> news_;
            std::vector<std::thread> threads_;

            void cleanup() {
                if (needs_cleanup_) {
                    for (auto& t : threads_) {
                        t.join();
                    }
                    for (auto& ptr : news_) {
                        delete[] ptr;
                    }
					file_.close();
                #ifdef _WIN32
					closesocket(sockfd_);
                #else
					close(sockfd_);
                #endif
                    needs_cleanup_ = false;
                }
            }
        };
    };

    namespace {
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
    }
}




