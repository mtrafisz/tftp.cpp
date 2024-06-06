#include <iostream>
#include <fstream>
#include "../inc/tftp.hpp"
#include <ctime>
#include <sstream>
#ifndef _WIN32
#include <sys/time.h>
#endif

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
    } catch (const tftpc::TftpError& e) {
        std::cerr << e << std::endl;
        return 1;
    }

    std::cout << "Time taken: " << interval << "s" << std::endl;
}
