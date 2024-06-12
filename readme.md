# tftp.cpp - Full TFTP implementation in C++

Full client & server implementation - made to be minimal but featureful.

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
void tftpc::Client::send(const struct sockaddr_in& remote_addr, const std::string& filename, std::istream& data);

std::streamsize tftpc::Client::recv(const struct sockaddr_in& remote_addr, const std::string& filename, std::ostream& data);

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
- [ ] Progress reporting mechanism
