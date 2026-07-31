#pragma once

#include "database.hpp"
#include "resp.hpp"
#include <chrono>
#include <expected>
#include <optional>
#include <string>

namespace rlist {

struct BlockingPopRequest {
    std::string key;
    std::optional<std::chrono::milliseconds> timeout;
};

[[nodiscard]] std::expected<BlockingPopRequest, resp::SimpleError>
parse_blpop(const resp::Command& command);

[[nodiscard]]
resp::Response rpush(Database& database, const resp::Command& command);

[[nodiscard]]
resp::Response lpush(Database& database, const resp::Command& command);

[[nodiscard]]
resp::Response lrange(Database& database, const resp::Command& command);

[[nodiscard]]
resp::Response llen(Database& database, const resp::Command& command);

[[nodiscard]]
resp::Response lpop(Database& database, const resp::Command& command);

[[nodiscard]]
resp::Response blpop(Database& database, const resp::Command& command);
} // namespace rlist
