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
#include <istream>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <queue>
#include <fstream>
#include <filesystem>

namespace tftpc {

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

    /* Things You can edit, to change how library works: */

    // if defined, will use parallel file read, which is faster but uses more memory (up to file size at once at peak)
	// in other case, data will be read from file as needed (one chunk at the time) - transfer will be limited by disk read speed
    #define USE_PARALLEL_FILE_READ
	// if defined, it will use tftp extensions rfc to negotiate transfer parameters (blksize, timeout, file size)
	// TODO: enforce - for now not implemented and enabled by default
    #define NEGOTIATE_OPTIONS

    struct Config {
		// TODO thing crashes with 32k block size XD
		static constexpr uint16_t BlockSize = 16384;     // smaller -> better for smaller files and bad connections but transfers slow down considerably
        static constexpr uint16_t Timeout = 5;
        static constexpr uint16_t MaxRetries = 5;
		// TODO: add limit to queue size? - max memory usage limiter
    };
    // thats it :)

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

    class ServerResult {
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
		friend std::ostream& operator<<(std::ostream& os, const ServerResult& result) {
			// address:port read from/wrote to filename (bytes)
			os << inet_ntoa(result.client_addr.sin_addr) << ":" << ntohs(result.client_addr.sin_port) << " ";
            switch (result.type) {
			case Type::Read: os << "read from "; break;
			case Type::Write: os << "wrote to "; break;
			default: os << "unknown action on "; break;
            }
			os << result.filename << " (" << result.bytes_transferred << " bytes)";
			return os;
	    }
	};

    class Server {
    public:
        static ServerResult handleClient(socket_t sockfd, const std::string& root_dir);

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

        private:
            socket_t sockfd_;
            bool needs_cleanup_;
            std::vector<void*> news_;

            void cleanup() {
                if (needs_cleanup_) {
                    for (auto& ptr : news_) {
                        delete[] ptr;
                    }

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
}




