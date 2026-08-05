#include "server.hpp"
#include "server_config.hpp"
#include <arpa/inet.h>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <netdb.h>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    auto config = parse_arguments(argc, argv);

    if (!config) {
        std::println(stderr, "{}", config.error());
        return 1;
    }

    TcpServer server(std::move(*config));

    auto persistence_result = server.initialize_persistence();

    if (!persistence_result) {
        std::println(stderr, "{}", persistence_result.error());
        return 1;
    }

    auto server_setup_result = server.setup_server();

    if (!server_setup_result.has_value()) {
        std::println(stderr, "{}", server_setup_result.error());
        return 1;
    }

    auto replication_setup_result = server.setup_replication();

    if (!replication_setup_result.has_value()) {
        std::println(stderr, "{}", replication_setup_result.error());
        return 1;
    }

    // Runs the connection accept loop
    server.run();

    return 0;
}
