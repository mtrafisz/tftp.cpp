#include "../inc/tftp.hpp"
#include <ctime>
#include <csignal>
#include <sstream>
#ifndef _WIN32
#include <sys/time.h>
#endif

#define TIMEOUT_SECS 3

volatile bool keepRunning = true;

void signalHandler(int signum) {
	std::cerr << std::endl << "Ctrl+C detected, exiting..." << std::endl;
	keepRunning = false;
}

int main(void) {
	signal(SIGINT, signalHandler);
	int ret_code = 0;

#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		std::cerr << "WSAStartup failed" << std::endl;
		return 1;
	}
#endif
	
	int sockfd;
	if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		std::cerr << "socket creation failed" << std::endl;
		return 1;
	}
	struct sockaddr_in local_addr = {};
	local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	local_addr.sin_port = htons(6969);
	local_addr.sin_family = AF_INET;

	if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
		std::cerr << "bind failed" << std::endl;
		ret_code = 1;
		goto cleanup;
	}

#ifdef _WIN32
	const DWORD timeout = TIMEOUT_SECS * 1000;
#else
	const struct timeval timeout = { TIMEOUT_SECS, 0 };
#endif
	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

	while (keepRunning) {
		try {
			std::cout << tftp::Server::handleClient(sockfd, "./") << std::endl;
		}
		catch (const tftp::TftpError& e) {
			if (e.getCode() == TIMEOUT_OS_ERR) {
				std::cout << ".";
			}
			else {
				std::cerr << e << std::endl;
			}
		}
	}

cleanup:
#ifdef _WIN32
	WSACleanup();
	closesocket(sockfd);
#else
	close(sockfd);
#endif
}
