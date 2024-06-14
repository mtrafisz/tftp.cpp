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

tftp::Client::ProgressCallback progress_callback = [](tftp::Client::Progress& progress) {
    std::cout << "Progress: " << progress.transferred_bytes << " / " << progress.total_bytes << std::endl;
};

int main(void) {
    struct sockaddr_in remote_addr = {};
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(69);
    inet_pton(AF_INET, "127.0.0.1", &remote_addr.sin_addr);

    const std::string test_filename = "debian.iso"; // ~ 630 MB
    std::streamsize rcvd_size = 0;

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

     std::ofstream ofs(test_filename, std::ios::binary);

    try {

        #ifdef _WIN32
		        QueryPerformanceCounter(&start);
        #else
		        clock_gettime(CLOCK_MONOTONIC, &start);
        #endif

		rcvd_size = tftp::Client::recv("127.0.0.1:69", test_filename, ofs, progress_callback, std::chrono::abs(std::chrono::milliseconds(100)));
        // rcvd_size = tftp::Client::recv("127.0.0.1:69", test_filename, ofs);

    #ifdef _WIN32
		QueryPerformanceCounter(&end);
		interval = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    #else
		clock_gettime(CLOCK_MONOTONIC, &end);
		interval = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    #endif
		mbps = (float)(rcvd_size / 1e6) / (float)interval;
		std::cout << std::fixed << "Received in: " << interval << "s (" << mbps << "MBps)" << std::endl;

         ofs.close();

        std::ifstream file(test_filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open file" << std::endl;
            return 1;
        }
        std::streamsize send_size = gss(file);

    #ifdef _WIN32
		QueryPerformanceCounter(&start);
    #else
		clock_gettime(CLOCK_MONOTONIC, &start);
    #endif
        
		tftp::Client::send("127.0.0.1:69", test_filename, file, progress_callback, std::chrono::abs(std::chrono::milliseconds(100)));
        // tftp::Client::send("127.0.0.1:69", test_filename, file);

        #ifdef _WIN32
            QueryPerformanceCounter(&end);
            interval = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
        #else
            clock_gettime(CLOCK_MONOTONIC, &end);
            interval = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
        #endif
		mbps = (float)(send_size / 1e6) / (float)interval;
		std::cout << std::fixed << "Sent in: " << interval << "s (" << mbps << "MBps)" << std::endl;

    } catch (const tftp::TftpError& e) {
        std::cerr << e << std::endl;
         ofs.close();
        return 1;
    }

	std::cout << "Success!" << std::endl;

}
