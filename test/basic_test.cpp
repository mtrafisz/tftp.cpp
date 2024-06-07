#include <iostream>
#include <fstream>
#include "../inc/tftp.hpp"
#include <ctime>
#include <sstream>
#ifndef _WIN32
#include <sys/time.h>
#endif

std::streamsize gss(std::istream& stream) {
    std::streamsize current = stream.tellg();
    stream.seekg(0, std::ios::end);
    std::streamsize length = stream.tellg();

    stream.seekg(current, std::ios::beg);
    return length;
}

int main(void) {
    struct sockaddr_in remote_addr = {};
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(69);
    remote_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    std::ifstream file("../../../test.zip", std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open file" << std::endl;
        return 1;
    }
	std::streamsize send_size = gss(file);

	std::ofstream ofs("test.zip", std::ios::binary);

    float mbps;
    double interval;
    #ifdef _WIN32
        LARGE_INTEGER frequency;
        LARGE_INTEGER start;
        LARGE_INTEGER end;

        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    #else
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
    #endif

    try {
        tftpc::Client::send(remote_addr, "test.zip", file);

        #ifdef _WIN32
            QueryPerformanceCounter(&end);
            interval = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
        #else
            clock_gettime(CLOCK_MONOTONIC, &end);
            interval = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        #endif
		mbps = (send_size / 1e6) / interval;
		std::cout << std::fixed << "Sent in: " << interval << "s (" << mbps << "MBps)" << std::endl;

        #ifdef _WIN32
		        QueryPerformanceCounter(&start);
        #else
		        clock_gettime(CLOCK_MONOTONIC, &start);
        #endif

	    std::streamsize rcvd_size = tftpc::Client::recv(remote_addr, "test.zip", ofs);

    #ifdef _WIN32
		QueryPerformanceCounter(&end);
		interval = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    #else
		clock_gettime(CLOCK_MONOTONIC, &end);
		interval = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    #endif
		mbps = (rcvd_size / 1e6) / interval;
		std::cout << std::fixed << "Received in: " << interval << "s (" << mbps << "MBps)" << std::endl;

        ofs.close();
    } catch (const tftpc::TftpError& e) {
        std::cerr << e << std::endl;
        ofs.close();
        return 1;
    }

	std::cout << "Success!" << std::endl;

}
