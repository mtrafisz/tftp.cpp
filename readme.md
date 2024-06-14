# tftp.cpp - Full TFTP implementation in C++

Full client & server implementation - ~~made to be minimal but featureful.~~ HOW IS THE CLIENT ALONE SO BIG ALREADY

## Building

### Using CMake

```sh
mkdir build
cd build
cmake .. -G YourFavouriteGenerator
```

## Configuration

tftp.hpp exposes some macros and `struct Config` with things you can change to alter behaviour of the library.

## Api

```cpp
class tftp::Client::Progress {
public:
    size_t total_bytes;
    size_t transferred_bytes;
    
    Progress(size_t total_bytes) : total_bytes(total_bytes), transferred_bytes(0) {}
}

typedef std::function<void(tftp::Client::Progress&)> tftp::Client::ProgressCallback;

void tftp::Client::send (
    const std::string& remote_addr,
    const std::string& filename,
    std::istream& data,
    ProgressCallback progress = nullptr,
    std::chrono::milliseconds callback_interval = std::chrono::milliseconds(1000));

std::streamsize tftp::Client::recv (
    const std::string& remote_addr,
    const std::string& filename,
    std::ostream& data,
    ProgressCallback progress = nullptr,
    std::chrono::milliseconds callback_interval = std::chrono::milliseconds(1000));

ServerResult tftpc::Server::handleClient(socket_t sockfd, const std::string& root_dir);
```

More info in ~~[docs](docs.md)~~ Not done yet

## Info

Client functions managed to get to around 150 MB/s with blksize of 8192 bytes when transfering ~1GB .zip:

```bash
Received in: 6.511203s (152.563232MBps)
Sent in: 6.723765s (147.740158MBps)
```

## Todo

- [ ] Documentation
- [ ] Server rewrite
- [X] Progress reporting mechanism
