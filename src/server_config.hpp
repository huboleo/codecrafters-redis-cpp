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

struct AOFConfig {
    bool enabled{false};
    std::string append_dirname{"appendonlydir"};
    std::string append_filename{"appendonly.aof"};
    std::string append_fsync{"everysec"};
};

struct ServerConfig {
    std::uint16_t port{6379};
    std::optional<ReplicaConfig> replica_of;
    std::optional<std::string> default_user_password_hash;
    RDBConfig rdb_config;
    AOFConfig aof_config;
};

[[nodiscard]] std::expected<ServerConfig, std::string> parse_arguments(int argc, char** argv);
