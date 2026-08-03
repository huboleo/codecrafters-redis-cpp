#pragma once
#include "command_processor.hpp"
#include "resp.hpp"
#include "server_config.hpp"
#include <cstdint>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

class TcpServer {
  public:
    explicit TcpServer(ServerConfig config);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    // Returns server socket file descriptor if setup was successful;
    std::expected<void, std::string> setup_server();
    std::expected<void, std::string> setup_replication();
    void run();

  private:
    ServerConfig _config;
    std::uint64_t _next_client_id = 1;
    int _server_fd = -1;
    CommandProcessor _processor;
    std::optional<int> _master_fd;
    std::string _master_input_buffer;
    std::jthread _replication_thread;

    void run_connection_loop();

    void replication_loop(std::stop_token stop_token);

    std::expected<int, std::string> connect_to_master(const ReplicaConfig& replica_config);

    std::expected<void, std::string> perform_replication_handshake();

    std::expected<void, std::string> send_command(int socket_fd, resp::Command command);

    std::expected<std::string, std::string> read_master_response_line(int socket_fd);

    std::expected<void, std::string>
    send_command_and_expect(int socket_fd, resp::Command command,
                            std::string_view expected_response);

    void handle_connection(int client_fd, std::uint64_t client_id);
};
