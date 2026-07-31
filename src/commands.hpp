#pragma once

#include "database.hpp"
#include "resp.hpp"

namespace commands {
[[nodiscard]]
resp::Response ping(const resp::Command& command);

[[nodiscard]]
resp::Response echo(const resp::Command& command);

[[nodiscard]]
resp::Response set(Database& database, const resp::Command& command);

[[nodiscard]]
resp::Response get(Database& database, const resp::Command& command);

[[nodiscard]]
resp::Response type(Database& database, const resp::Command& command);

[[nodiscard]]
resp::Response incr(Database& database, const resp::Command& command);

} // namespace commands
