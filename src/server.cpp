#include "server.hpp"
#include "command_processor.hpp"
#include "resp.hpp"
#include "server_config.hpp"
#include <arpa/inet.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>

TcpServer::TcpServer(ServerConfig config)
    : _config(std::move(config)), _processor(_config.replica_of) {}

TcpServer::~TcpServer() {
    if (_server_fd >= 0) {
        close(_server_fd);
    }
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

void TcpServer::run() {
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
