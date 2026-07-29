#pragma once
#include "command_executor.hpp"
#include <cstdint>
#include <expected>
#include <string>

class TcpServer {
  public:
    explicit TcpServer(std::uint16_t port);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    // Returns server socket file descriptor if setup was successful;
    std::expected<void, std::string> setup_server();
    void run();

  private:
    int _server_fd = -1;
    std::uint16_t _port;
    CommandExecutor _executor;
    void handle_connection(int client_fd);
};
