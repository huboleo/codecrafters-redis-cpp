#include "server_config.hpp"
#include <charconv>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace {
std::expected<std::uint16_t, std::string> parse_port_number(std::string_view arg) {
    std::uint16_t port{};

    const auto [end, error] = std::from_chars(arg.data(), arg.data() + arg.size(), port);

    if (arg.empty() || error != std::errc{} || end != arg.data() + arg.size() || port == 0) {
        return std::unexpected("Invalid port number");
    }

    return port;
}

std::expected<ReplicaConfig, std::string> parse_replica_config(std::string_view arg) {
    const auto separator = arg.find(' ');

    if (separator == std::string_view::npos) {
        return std::unexpected("Expected --replicaof \"<host> <port>\"");
    }

    const std::string_view host = arg.substr(0, separator);
    const std::string_view port_text = arg.substr(separator + 1);

    if (host.empty() || port_text.empty()) {
        return std::unexpected("Invalid --replicaof value");
    }

    auto port = parse_port_number(port_text);

    if (!port) {
        return std::unexpected("Invalid replica port");
    }

    return ReplicaConfig{
        .host = std::string{host},
        .port = *port,
    };
}

} // namespace

std::expected<ServerConfig, std::string> parse_arguments(int argc, char** argv) {
    ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};

        if (arg == "--port") {
            if (i + 1 >= argc) {
                return std::unexpected("Missing value after --port");
            }

            auto port = parse_port_number(std::string_view{argv[++i]});

            if (!port) {
                return std::unexpected(port.error());
            }

            config.port = *port;
            continue;
        }

        if (arg == "--replicaof") {
            if (i + 1 >= argc) {
                return std::unexpected("Missing value after --replicaof");
            }

            auto replica = parse_replica_config(std::string_view{argv[++i]});

            if (!replica) {
                return std::unexpected(replica.error());
            }

            config.replica_of = std::move(*replica);
            continue;
        }

        return std::unexpected("Unknown argument: " + std::string{arg});
    }

    return config;
}
