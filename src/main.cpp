#include "server.hpp"
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

std::expected<std::uint16_t, std::string> parse_port_number(const char* arg) {
    if (arg == nullptr) {
        return std::unexpected("Port argument is missing");
    }

    const std::string_view text{arg};
    std::uint16_t port{};

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);

    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || port == 0) {
        return std::unexpected("Invalid port number");
    }

    return port;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::uint16_t port = 6379;

    if (argc >= 3) {
        auto parsed_port = parse_port_number(argv[2]);

        if (!parsed_port) {
            std::println(stderr, "{}", parsed_port.error());
            return 1;
        }

        port = *parsed_port;
    }

    TcpServer server(port);

    auto setup_result = server.setup_server();

    if (!setup_result.has_value()) {
        std::println(stderr, "{}", setup_result.error());
        return 1;
    }

    // Runs the connection accept loop
    server.run();

    return 0;
}
