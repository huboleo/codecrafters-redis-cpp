#include "server.hpp"
#include "resp.hpp"
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <netdb.h>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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
    server_addr.sin_port = htons(6379);

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

void TcpServer::accept_connections() {
    while (true) {
        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);
        std::println("Waiting for a client to connect...");

        int client_socket_fd =
            accept(_server_fd, (struct sockaddr*)&client_addr, (socklen_t*)&client_addr_len);
        if (client_socket_fd < 0) {
            continue;
        }

        std::println("Client connected");

        auto response = resp::serialize_string_response(resp::RespDataType::SIMPLE_STRING, "PONG");

        send(client_socket_fd, response.c_str(), response.size(), 0);
    }
}
