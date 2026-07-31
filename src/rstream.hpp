#pragma once

#include "database.hpp"
#include "resp.hpp"
#include "rstream_types.hpp"
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

namespace rstream {
std::optional<IdRequest> parse_id(std::string_view id);

[[nodiscard]] std::expected<ReadCommand, resp::SimpleError>
parse_xread(const resp::Command& command);

[[nodiscard]] resp::Response make_xread_response(std::vector<ReadResult> results);

[[nodiscard]] resp::Response xadd(Database& database, const resp::Command& command);

[[nodiscard]] resp::Response xrange(Database& database, const resp::Command& command);

[[nodiscard]] resp::Response xread(Database& database, const resp::Command& command);

} // namespace rstream
