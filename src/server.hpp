#pragma once
#include <expected>
#include <string>

class TcpServer {
  public:
    TcpServer() = default;
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    // Returns server socket file descriptor if setup was successful;
    std::expected<void, std::string> setup_server();
    void accept_connections();

  private:
    int _server_fd = -1;
};
