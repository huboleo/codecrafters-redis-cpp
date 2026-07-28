#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace time_utils {
std::optional<std::chrono::milliseconds::rep> parse_expiry(const std::string& text);
std::optional<std::chrono::seconds::rep> parse_blocking_timeout(const std::string& text);
} // namespace time_utils
