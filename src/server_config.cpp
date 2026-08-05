#include "server_config.hpp"
#include <charconv>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

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

std::expected<void, std::string> validate_directory(std::string_view path) {
    if (path.empty()) {
        return std::unexpected("RDB directory cannot be empty");
    }

    std::error_code error;
    const auto status = std::filesystem::status(path, error);

    if (error) {
        return std::unexpected("Cannot access RDB directory '" + std::string{path} +
                               "': " + error.message());
    }

    if (!std::filesystem::is_directory(status)) {
        return std::unexpected("RDB path '" + std::string{path} + "' is not a directory");
    }

    return {};
}

bool is_valid_database_filename(std::string_view filename) {
    if (filename.empty()) {
        return false;
    }

    const std::filesystem::path path{filename};

    return !path.has_parent_path() && path != "." && path != "..";
}

bool is_valid_append_directory(std::string_view directory) {
    if (directory.empty()) {
        return false;
    }

    const std::filesystem::path path{directory};

    if (path.is_absolute() || path == "." || path == "..") {
        return false;
    }

    for (const auto& component : path) {
        if (component == "..") {
            return false;
        }
    }

    return true;
}

} // namespace

std::expected<ServerConfig, std::string> parse_arguments(int argc, char** argv) {
    ServerConfig config;
    std::error_code current_path_error;
    const std::filesystem::path current_path =
        std::filesystem::current_path(current_path_error);

    if (current_path_error) {
        return std::unexpected("Cannot determine the current working directory: " +
                               current_path_error.message());
    }

    config.rdb_config.dir = current_path.string();

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

        if (arg == "--dir") {
            if (i + 1 >= argc) {
                return std::unexpected("Missing value after --dir");
            }

            const std::string_view path{argv[++i]};

            auto validation = validate_directory(path);

            if (!validation) {
                return std::unexpected(validation.error());
            }

            config.rdb_config.dir = path;
            continue;
        }

        if (arg == "--dbfilename") {
            if (i + 1 >= argc) {
                return std::unexpected("Missing value after --dbfilename");
            }

            const std::string_view filename{argv[++i]};

            if (!is_valid_database_filename(filename)) {
                return std::unexpected("RDB filename must be a filename without a directory path");
            }

            config.rdb_config.db_filename = filename;
            continue;
        }

        if (arg == "--appendonly") {
            if (i + 1 >= argc) {
                return std::unexpected("Missing value after --appendonly");
            }

            const std::string_view value{argv[++i]};

            if (value != "yes" && value != "no") {
                return std::unexpected("--appendonly must be either 'yes' or 'no'");
            }

            config.aof_config.enabled = value == "yes";
            continue;
        }

        if (arg == "--appenddirname") {
            if (i + 1 >= argc) {
                return std::unexpected("Missing value after --appenddirname");
            }

            const std::string_view directory{argv[++i]};

            if (!is_valid_append_directory(directory)) {
                return std::unexpected(
                    "AOF directory must be a relative path inside the data directory");
            }

            config.aof_config.append_dirname = directory;
            continue;
        }

        if (arg == "--appendfilename") {
            if (i + 1 >= argc) {
                return std::unexpected("Missing value after --appendfilename");
            }

            const std::string_view filename{argv[++i]};

            if (!is_valid_database_filename(filename)) {
                return std::unexpected("AOF filename must be a filename without a directory path");
            }

            config.aof_config.append_filename = filename;
            continue;
        }

        if (arg == "--appendfsync") {
            if (i + 1 >= argc) {
                return std::unexpected("Missing value after --appendfsync");
            }

            const std::string_view policy{argv[++i]};

            if (policy != "always" && policy != "everysec" && policy != "no") {
                return std::unexpected(
                    "--appendfsync must be 'always', 'everysec', or 'no'");
            }

            config.aof_config.append_fsync = policy;
            continue;
        }

        return std::unexpected("Unknown argument: " + std::string{arg});
    }

    return config;
}
