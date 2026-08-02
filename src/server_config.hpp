#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

struct ReplicaConfig {
    std::string host;
    std::uint16_t port;
};

struct ServerConfig {
    std::uint16_t port{6379};
    std::optional<ReplicaConfig> replica_of;
};

[[nodiscard]] std::expected<ServerConfig, std::string> parse_arguments(int argc, char** argv);
