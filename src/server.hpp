#pragma once
#include "command_executor.hpp"
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
    void run();

  private:
    int _server_fd = -1;
    CommandExecutor _executor{};
    void handle_connection(int client_fd);
};
