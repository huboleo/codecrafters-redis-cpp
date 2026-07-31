#pragma once
#include "command_processor.hpp"
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
    std::uint64_t _next_client_id = 1;
    int _server_fd = -1;
    std::uint16_t _port;
    CommandProcessor _processor;
    void handle_connection(int client_fd, std::uint64_t client_id);
};
