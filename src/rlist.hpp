#pragma once

#include "database.hpp"
#include "resp.hpp"

namespace rlist {

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
