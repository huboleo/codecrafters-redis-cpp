#include "time.hpp"
#include <charconv>
#include <chrono>
#include <cmath>
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

std::optional<double> time_utils::parse_blocking_timeout(const std::string& text) {
    double seconds{};

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds,
                                              std::chars_format::general);

    if (text.empty() or error != std::errc{} or end != text.data() + text.size() or
        !std::isfinite(seconds) or seconds < 0.0) {
        return std::nullopt;
    }

    return seconds;
}
