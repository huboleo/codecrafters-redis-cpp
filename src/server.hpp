#pragma once
#include "command_processor.hpp"
#include "server_config.hpp"
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

class TcpServer {
  public:
    explicit TcpServer(ServerConfig config);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    // Returns server socket file descriptor if setup was successful;
    std::expected<void, std::string> setup_server();
    void run();

  private:
    ServerConfig _config;
    std::uint64_t _next_client_id = 1;
    int _server_fd = -1;
    CommandProcessor _processor;
    void handle_connection(int client_fd, std::uint64_t client_id);
};
