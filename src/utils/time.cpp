#include "time.hpp"
#include <charconv>
#include <chrono>
#include <optional>
#include <string>

std::optional<std::chrono::milliseconds::rep> time_utils::parse_expiry(const std::string& text) {
    std::chrono::milliseconds::rep count{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);

    if (error != std::errc{} or end != text.data() + text.size() or count <= 0) {
        return std::nullopt;
    }

    return count;
}

std::optional<std::chrono::seconds::rep>
time_utils::parse_blocking_timeout(const std::string& text) {
    std::chrono::seconds::rep count{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);

    if (error != std::errc{} or end != text.data() + text.size() or count < 0) {
        return std::nullopt;
    }

    return count;
}
