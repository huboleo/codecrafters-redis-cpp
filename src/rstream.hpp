#pragma once

#include "database.hpp"
#include "resp.hpp"
#include "rstream_types.hpp"
#include <optional>
#include <string_view>

namespace rstream {
std::optional<IdRequest> parse_id(std::string_view id);

[[nodiscard]] resp::Response xadd(Database& database, const resp::Command& command);

} // namespace rstream
