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
    }

    TcpServer server(std::move(*config));

    auto setup_result = server.setup_server();

    if (!setup_result.has_value()) {
        std::println(stderr, "{}", setup_result.error());
        return 1;
    }

    // Runs the connection accept loop
    server.run();

    return 0;
}
