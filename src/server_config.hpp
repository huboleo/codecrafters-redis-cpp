#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

struct ReplicaConfig {
    std::string host;
    std::uint16_t port;
};

struct RDBConfig {
    std::string dir{"."};
    std::string db_filename{"dump.rdb"};
};

struct ServerConfig {
    std::uint16_t port{6379};
    std::optional<ReplicaConfig> replica_of;
    RDBConfig rdb_config;
};

[[nodiscard]] std::expected<ServerConfig, std::string> parse_arguments(int argc, char** argv);
