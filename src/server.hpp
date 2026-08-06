#pragma once
#include "command_processor.hpp"
#include "resp.hpp"
#include "server_config.hpp"
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class TcpServer {
  public:
    explicit TcpServer(ServerConfig config);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    // Returns server socket file descriptor if setup was successful;
    std::expected<void, std::string> setup_server();
    std::expected<void, std::string> setup_replication();
    std::expected<void, std::string> initialize_persistence();
    void run();
    void stop();

  private:
    struct FullResync {
        std::string replication_id;
        std::uint64_t offset;
    };

    struct ConnectionState {
        ConnectionState(std::uint64_t client_id, int socket_fd)
            : client_id(client_id), socket_fd(socket_fd) {}

        std::uint64_t client_id;
        std::atomic<int> socket_fd;
        std::atomic_bool finished{false};
    };

    struct ConnectionWorker {
        std::shared_ptr<ConnectionState> state;
        std::jthread thread;
    };

    ServerConfig _config;
    std::uint64_t _next_client_id = 1;
    int _server_fd = -1;
    CommandProcessor _processor;
    std::optional<int> _master_fd;
    std::string _master_input_buffer;
    std::jthread _replication_thread;
    std::atomic_bool _stopping{false};
    std::mutex _stop_mutex;
    std::mutex _connections_mutex;
    std::vector<ConnectionWorker> _connection_workers;

    void run_connection_loop();

    void start_connection(int socket_fd, std::uint64_t client_id);

    void reap_completed_connections();

    void stop_connection_workers();

    void replication_loop(std::stop_token stop_token);

    std::expected<int, std::string> connect_to_master(const ReplicaConfig& replica_config);

    std::expected<void, std::string> perform_replication_handshake();

    std::expected<FullResync, std::string> receive_full_resync();

    std::expected<std::string, std::string> receive_rdb_payload();

    std::expected<void, std::string>
    consume_replication_stream(std::stop_token stop_token);

    std::expected<void, std::string> send_command(int socket_fd,
                                                  const resp::Command& command);

    std::expected<void, std::string> send_bytes(int socket_fd, std::string_view bytes);

    std::expected<void, std::string>
    send_full_resync(int socket_fd, const ReplicaRegistration& registration);

    std::expected<void, std::string>
    run_replica_connection(int socket_fd, ReplicaRegistration registration);

    void send_replication_frames(int socket_fd,
                                 std::shared_ptr<ReplicaSession> session,
                                 std::stop_token stop_token);

    std::expected<void, std::string>
    consume_replica_acknowledgements(int socket_fd, std::uint64_t client_id,
                                     std::stop_token stop_token);

    std::expected<std::string, std::string> read_master_response_line(int socket_fd);

    std::expected<void, std::string>
    send_command_and_expect(int socket_fd, const resp::Command& command,
                            std::string_view expected_response);

    void handle_connection(int client_fd, std::uint64_t client_id,
                           std::stop_token stop_token);
};
