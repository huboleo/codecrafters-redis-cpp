#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rdb {

struct StringEntry {
    std::string key;
    std::string value;
    std::optional<std::uint64_t> expires_at_unix_ms;
};

[[nodiscard]] std::expected<std::vector<StringEntry>, std::string>
read_file(const std::filesystem::path& path);

} // namespace rdb
