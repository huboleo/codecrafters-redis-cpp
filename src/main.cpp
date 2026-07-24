#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::println(stderr, "Failed to create server socket");
        return 1;
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::println(stderr, "setsockopt failed");
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(6379);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
        std::println(stderr, "Failed to bind to port 6379");
        return 1;
    }

    int connection_backlog = 5;
    if (listen(server_fd, connection_backlog) != 0) {
        std::println(stderr, "listen failed");
        return 1;
    }

    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);
    std::println("Waiting for a client to connect...");

    accept(server_fd, (struct sockaddr*)&client_addr, (socklen_t*)&client_addr_len);
    std::println("Client connected");
    close(server_fd);

    return 0;
}
