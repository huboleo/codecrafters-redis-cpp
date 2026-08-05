#include "rdb.hpp"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::uint8_t string_type = 0;

constexpr std::uint8_t idle_opcode = 248;
constexpr std::uint8_t frequency_opcode = 249;
constexpr std::uint8_t auxiliary_opcode = 250;
constexpr std::uint8_t resize_database_opcode = 251;
constexpr std::uint8_t expire_time_ms_opcode = 252;
constexpr std::uint8_t expire_time_seconds_opcode = 253;
constexpr std::uint8_t select_database_opcode = 254;
constexpr std::uint8_t end_of_file_opcode = 255;

struct Length {
    std::uint64_t value;
    bool encoded;
};

class Reader {
  public:
    explicit Reader(std::string_view input) : _input(input) {}

    [[nodiscard]] std::size_t remaining() const { return _input.size() - _position; }

    [[nodiscard]] std::expected<std::uint8_t, std::string> read_byte() {
        if (remaining() == 0) {
            return std::unexpected("Unexpected end of RDB file");
        }

        return static_cast<std::uint8_t>(_input[_position++]);
    }

    [[nodiscard]] std::expected<std::string_view, std::string>
    read_bytes(std::size_t count) {
        if (count > remaining()) {
            return std::unexpected("Unexpected end of RDB file");
        }

        const std::string_view bytes = _input.substr(_position, count);
        _position += count;
        return bytes;
    }

    [[nodiscard]] std::expected<std::uint32_t, std::string> read_u32_little_endian() {
        auto bytes = read_bytes(4);

        if (!bytes) {
            return std::unexpected(bytes.error());
        }

        std::uint32_t result = 0;

        for (std::size_t index = 0; index < bytes->size(); ++index) {
            result |= static_cast<std::uint32_t>(
                          static_cast<std::uint8_t>((*bytes)[index]))
                      << (index * 8);
        }

        return result;
    }

    [[nodiscard]] std::expected<std::uint64_t, std::string> read_u64_little_endian() {
        auto bytes = read_bytes(8);

        if (!bytes) {
            return std::unexpected(bytes.error());
        }

        std::uint64_t result = 0;

        for (std::size_t index = 0; index < bytes->size(); ++index) {
            result |= static_cast<std::uint64_t>(
                          static_cast<std::uint8_t>((*bytes)[index]))
                      << (index * 8);
        }

        return result;
    }

    [[nodiscard]] std::expected<Length, std::string> read_length() {
        auto first = read_byte();

        if (!first) {
            return std::unexpected(first.error());
        }

        const std::uint8_t kind = *first >> 6;

        if (kind == 0) {
            return Length{.value = *first & 0x3f, .encoded = false};
        }

        if (kind == 1) {
            auto second = read_byte();

            if (!second) {
                return std::unexpected(second.error());
            }

            return Length{
                .value = (static_cast<std::uint64_t>(*first & 0x3f) << 8) | *second,
                .encoded = false,
            };
        }

        if (kind == 3) {
            return Length{.value = *first & 0x3f, .encoded = true};
        }

        if (*first == 0x80) {
            auto bytes = read_bytes(4);

            if (!bytes) {
                return std::unexpected(bytes.error());
            }

            std::uint32_t result = 0;

            for (const char byte : *bytes) {
                result = (result << 8) | static_cast<std::uint8_t>(byte);
            }

            return Length{.value = result, .encoded = false};
        }

        if (*first == 0x81) {
            auto bytes = read_bytes(8);

            if (!bytes) {
                return std::unexpected(bytes.error());
            }

            std::uint64_t result = 0;

            for (const char byte : *bytes) {
                result = (result << 8) | static_cast<std::uint8_t>(byte);
            }

            return Length{.value = result, .encoded = false};
        }

        return std::unexpected("Unsupported RDB length encoding");
    }

    [[nodiscard]] std::expected<std::uint64_t, std::string> read_plain_length() {
        auto length = read_length();

        if (!length) {
            return std::unexpected(length.error());
        }

        if (length->encoded) {
            return std::unexpected("Expected an ordinary RDB length");
        }

        return length->value;
    }

    [[nodiscard]] std::expected<std::string, std::string> read_string() {
        auto length = read_length();

        if (!length) {
            return std::unexpected(length.error());
        }

        if (!length->encoded) {
            if (length->value > std::numeric_limits<std::size_t>::max()) {
                return std::unexpected("RDB string is too large");
            }

            auto bytes = read_bytes(static_cast<std::size_t>(length->value));

            if (!bytes) {
                return std::unexpected(bytes.error());
            }

            return std::string{*bytes};
        }

        if (length->value == 0) {
            auto value = read_byte();

            if (!value) {
                return std::unexpected(value.error());
            }

            return std::to_string(std::bit_cast<std::int8_t>(*value));
        }

        if (length->value == 1) {
            auto bytes = read_bytes(2);

            if (!bytes) {
                return std::unexpected(bytes.error());
            }

            const std::uint16_t value =
                static_cast<std::uint8_t>((*bytes)[0]) |
                (static_cast<std::uint16_t>(static_cast<std::uint8_t>((*bytes)[1])) << 8);

            return std::to_string(std::bit_cast<std::int16_t>(value));
        }

        if (length->value == 2) {
            auto value = read_u32_little_endian();

            if (!value) {
                return std::unexpected(value.error());
            }

            return std::to_string(std::bit_cast<std::int32_t>(*value));
        }

        return std::unexpected("LZF-compressed RDB strings are not supported");
    }

  private:
    std::string_view _input;
    std::size_t _position{};
};

[[nodiscard]] std::expected<std::vector<rdb::StringEntry>, std::string>
parse(std::string_view contents) {
    if (contents.size() < 10 || contents.substr(0, 5) != "REDIS") {
        return std::unexpected("Invalid RDB header");
    }

    for (const char character : contents.substr(5, 4)) {
        if (character < '0' || character > '9') {
            return std::unexpected("Invalid RDB version");
        }
    }

    Reader reader{contents};
    auto header = reader.read_bytes(9);

    if (!header) {
        return std::unexpected(header.error());
    }

    std::vector<rdb::StringEntry> entries;
    std::optional<std::uint64_t> pending_expiration;

    while (reader.remaining() > 0) {
        auto marker = reader.read_byte();

        if (!marker) {
            return std::unexpected(marker.error());
        }

        if (*marker == end_of_file_opcode) {
            return entries;
        }

        if (*marker == auxiliary_opcode) {
            auto key = reader.read_string();

            if (!key) {
                return std::unexpected(key.error());
            }

            auto value = reader.read_string();

            if (!value) {
                return std::unexpected(value.error());
            }

            continue;
        }

        if (*marker == select_database_opcode) {
            auto database = reader.read_plain_length();

            if (!database) {
                return std::unexpected(database.error());
            }

            if (*database != 0) {
                return std::unexpected("Only RDB database 0 is supported");
            }

            continue;
        }

        if (*marker == resize_database_opcode) {
            auto key_count = reader.read_plain_length();

            if (!key_count) {
                return std::unexpected(key_count.error());
            }

            auto expiring_key_count = reader.read_plain_length();

            if (!expiring_key_count) {
                return std::unexpected(expiring_key_count.error());
            }

            if (*key_count <= std::numeric_limits<std::size_t>::max()) {
                entries.reserve(static_cast<std::size_t>(*key_count));
            }

            continue;
        }

        if (*marker == expire_time_ms_opcode) {
            auto expiration = reader.read_u64_little_endian();

            if (!expiration) {
                return std::unexpected(expiration.error());
            }

            pending_expiration = *expiration;
            continue;
        }

        if (*marker == expire_time_seconds_opcode) {
            auto expiration = reader.read_u32_little_endian();

            if (!expiration) {
                return std::unexpected(expiration.error());
            }

            pending_expiration = static_cast<std::uint64_t>(*expiration) * 1000;
            continue;
        }

        if (*marker == idle_opcode) {
            auto idle = reader.read_plain_length();

            if (!idle) {
                return std::unexpected(idle.error());
            }

            continue;
        }

        if (*marker == frequency_opcode) {
            auto frequency = reader.read_byte();

            if (!frequency) {
                return std::unexpected(frequency.error());
            }

            continue;
        }

        if (*marker != string_type) {
            return std::unexpected("Only string values are supported in RDB files");
        }

        auto key = reader.read_string();

        if (!key) {
            return std::unexpected(key.error());
        }

        auto value = reader.read_string();

        if (!value) {
            return std::unexpected(value.error());
        }

        entries.push_back(rdb::StringEntry{
            .key = std::move(*key),
            .value = std::move(*value),
            .expires_at_unix_ms = pending_expiration,
        });
        pending_expiration.reset();
    }

    return std::unexpected("RDB file is missing its end marker");
}

} // namespace

std::expected<std::vector<rdb::StringEntry>, std::string>
rdb::read_file(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);

    if (error) {
        return std::unexpected("Cannot inspect RDB file '" + path.string() +
                               "': " + error.message());
    }

    if (!exists) {
        return std::vector<StringEntry>{};
    }

    std::ifstream file{path, std::ios::binary};

    if (!file) {
        return std::unexpected("Cannot open RDB file '" + path.string() + "'");
    }

    const std::string contents{std::istreambuf_iterator<char>{file},
                               std::istreambuf_iterator<char>{}};

    if (file.bad()) {
        return std::unexpected("Failed while reading RDB file '" + path.string() + "'");
    }

    return parse(contents);
}
