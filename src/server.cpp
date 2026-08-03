#include "server.hpp"
#include "command_processor.hpp"
#include "resp.hpp"
#include "server_config.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <netdb.h>
#include <print>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>

TcpServer::TcpServer(ServerConfig config)
    : _config(std::move(config)), _processor(_config.replica_of) {}

TcpServer::~TcpServer() {
    if (_replication_thread.joinable()) {
        _replication_thread.request_stop();

        if (_master_fd) {
            shutdown(*_master_fd, SHUT_RDWR);
        }

        _replication_thread.join();
    }

    if (_server_fd >= 0) {
        close(_server_fd);
    }

    if (_master_fd && *_master_fd >= 0) {
        close(*_master_fd);
    }
}

std::expected<int, std::string> TcpServer::connect_to_master(const ReplicaConfig& config) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(config.port);

    const int lookup_result = getaddrinfo(config.host.c_str(), port.c_str(), &hints, &addresses);

    if (lookup_result != 0) {
        return std::unexpected(std::string{"Failed to resolve master address: "} +
                               gai_strerror(lookup_result));
    }

    int master_fd = -1;

    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const int candidate_fd =
            socket(address->ai_family, address->ai_socktype, address->ai_protocol);

        if (candidate_fd < 0) {
            continue;
        }

        if (::connect(candidate_fd, address->ai_addr, address->ai_addrlen) == 0) {
            master_fd = candidate_fd;
            break;
        }

        close(candidate_fd);
    }

    freeaddrinfo(addresses);

    if (master_fd < 0) {
        return std::unexpected("Failed to connect to master " + config.host + ":" + port);
    }

    return master_fd;
}

std::expected<void, std::string> TcpServer::setup_server() {
    _server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_server_fd < 0) {
        return std::unexpected("Failed to create server socket");
    }

    int reuse = 1;
    if (setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        close(_server_fd);
        return std::unexpected("setsockopt failed");
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(_config.port);

    if (bind(_server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        close(_server_fd);
        return std::unexpected("Failed to bind to port 6379");
    }

    int connection_backlog = 5;
    if (listen(_server_fd, connection_backlog) != 0) {
        close(_server_fd);
        return std::unexpected("listen failed");
    }

    return {};
}

std::expected<void, std::string> TcpServer::setup_replication() {
    if (!_config.replica_of) {
        return {};
    }

    auto master_fd = connect_to_master(*_config.replica_of);

    if (!master_fd) {
        return std::unexpected(master_fd.error());
    }

    _master_fd = *master_fd;

    return {};
}

std::expected<void, std::string> TcpServer::send_command(int socket_fd,
                                                         resp::Command command) {
    const std::string serialized = resp::serialize_response(resp::Array{
        .values = std::move(command),
    });

    std::size_t bytes_sent = 0;

    while (bytes_sent < serialized.size()) {
        const ssize_t result =
            send(socket_fd, serialized.data() + bytes_sent, serialized.size() - bytes_sent, 0);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return std::unexpected("Failed to send command to master");
        }

        if (result == 0) {
            return std::unexpected("Master connection closed while sending command");
        }

        bytes_sent += static_cast<std::size_t>(result);
    }

    return {};
}

std::expected<std::string, std::string>
TcpServer::read_master_response_line(int socket_fd) {
    while (true) {
        const std::size_t line_end = _master_input_buffer.find("\r\n");

        if (line_end != std::string::npos) {
            std::string line = _master_input_buffer.substr(0, line_end);
            _master_input_buffer.erase(0, line_end + 2);
            return line;
        }

        char receive_buffer[1024];
        const ssize_t bytes_read = recv(socket_fd, receive_buffer, sizeof(receive_buffer), 0);

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return std::unexpected("Failed to read response from master");
        }

        if (bytes_read == 0) {
            return std::unexpected("Master closed the connection");
        }

        _master_input_buffer.append(receive_buffer, static_cast<std::size_t>(bytes_read));
    }
}

std::expected<void, std::string>
TcpServer::send_command_and_expect(int socket_fd, resp::Command command,
                                   std::string_view expected_response) {
    auto send_result = send_command(socket_fd, std::move(command));

    if (!send_result) {
        return send_result;
    }

    auto response = read_master_response_line(socket_fd);

    if (!response) {
        return std::unexpected(response.error());
    }

    if (*response != expected_response) {
        return std::unexpected(std::format("Expected response {}, received {}", expected_response,
                                           *response));
    }

    return {};
}

std::expected<void, std::string> TcpServer::perform_replication_handshake() {
    if (!_master_fd) {
        return std::unexpected("Master connection is not established");
    }

    auto result = send_command_and_expect(*_master_fd, {"PING"}, "+PONG");

    if (!result) {
        return result;
    }

    result = send_command_and_expect(
        *_master_fd, {"REPLCONF", "listening-port", std::to_string(_config.port)}, "+OK");

    if (!result) {
        return result;
    }

    result = send_command_and_expect(*_master_fd, {"REPLCONF", "capa", "psync2"}, "+OK");

    if (!result) {
        return result;
    }

    return send_command(*_master_fd, {"PSYNC", "?", "-1"});
}

std::expected<TcpServer::FullResync, std::string> TcpServer::receive_full_resync() {
    if (!_master_fd) {
        return std::unexpected("Master connection is not established");
    }

    auto response = read_master_response_line(*_master_fd);

    if (!response) {
        return std::unexpected(response.error());
    }

    constexpr std::string_view prefix = "+FULLRESYNC ";
    std::string_view payload{*response};

    if (!payload.starts_with(prefix)) {
        return std::unexpected(
            std::format("Expected FULLRESYNC response, received {}", *response));
    }

    payload.remove_prefix(prefix.size());

    const std::size_t separator = payload.find(' ');

    if (separator == std::string_view::npos) {
        return std::unexpected("Invalid FULLRESYNC response");
    }

    const std::string_view replication_id = payload.substr(0, separator);
    const std::string_view offset_text = payload.substr(separator + 1);

    if (replication_id.size() != 40 || offset_text.empty()) {
        return std::unexpected("Invalid FULLRESYNC response");
    }

    for (const char character : replication_id) {
        const bool is_digit = character >= '0' && character <= '9';
        const bool is_lowercase_hex = character >= 'a' && character <= 'f';
        const bool is_uppercase_hex = character >= 'A' && character <= 'F';

        if (!is_digit && !is_lowercase_hex && !is_uppercase_hex) {
            return std::unexpected("Invalid replication ID in FULLRESYNC response");
        }
    }

    std::uint64_t offset{};
    const auto [end, error] = std::from_chars(
        offset_text.data(), offset_text.data() + offset_text.size(), offset);

    if (error != std::errc{} || end != offset_text.data() + offset_text.size()) {
        return std::unexpected("Invalid replication offset in FULLRESYNC response");
    }

    return FullResync{
        .replication_id = std::string{replication_id},
        .offset = offset,
    };
}

void TcpServer::handle_connection(int client_fd, std::uint64_t client_id) {
    std::string input_buffer;
    char receive_buffer[1024];
    while (true) {
        ssize_t bytes_read = recv(client_fd, receive_buffer, sizeof(receive_buffer), 0);

        if (bytes_read <= 0) {
            break;
        }

        input_buffer.append(receive_buffer, static_cast<std::size_t>(bytes_read));

        while (!input_buffer.empty()) {
            auto outcome = resp::parse_command(input_buffer);

            if (std::holds_alternative<resp::Incomplete>(outcome)) {
                break;
            }

            if (auto* error = std::get_if<resp::ParseError>(&outcome)) {
                close(client_fd);
                return;
            }

            auto& parse_result = std::get<resp::ParseResult>(outcome);

            auto future_result = _processor.submit(client_id, std::move(parse_result.command));

            auto result = future_result.get();

            auto serialized_response = resp::serialize_response(std::move(result));

            send(client_fd, serialized_response.data(), serialized_response.size(), 0);

            input_buffer.erase(0, parse_result.bytes_consumed);
        }
    }

    close(client_fd);
}

void TcpServer::run_connection_loop() {
    while (true) {
        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);

        const int client_socket_fd =
            accept(_server_fd, (struct sockaddr*)&client_addr, (socklen_t*)&client_addr_len);

        if (client_socket_fd < 0) {
            continue;
        }

        const std::uint64_t client_id = _next_client_id++;

        std::thread(&TcpServer::handle_connection, this, client_socket_fd, client_id).detach();
    }
}

void TcpServer::replication_loop(std::stop_token stop_token) {
    auto handshake_result = perform_replication_handshake();

    if (!handshake_result) {
        if (!stop_token.stop_requested()) {
            std::println(stderr, "Replication handshake failed: {}", handshake_result.error());
        }

        return;
    }

    if (stop_token.stop_requested()) {
        return;
    }

    auto full_resync = receive_full_resync();

    if (!full_resync) {
        if (!stop_token.stop_requested()) {
            std::println(stderr, "Failed to receive FULLRESYNC: {}", full_resync.error());
        }

        return;
    }

    if (stop_token.stop_requested()) {
        return;
    }

    // The RDB payload and propagated command stream will be handled here. The parsed replication
    // ID and offset remain available in full_resync for those stages.
}

void TcpServer::run() {
    if (_master_fd) {
        _replication_thread = std::jthread(
            [this](std::stop_token stop_token) { replication_loop(stop_token); });
    }

    run_connection_loop();
}
