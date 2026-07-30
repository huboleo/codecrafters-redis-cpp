#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rstream {

struct StreamId {
    std::uint64_t milliseconds;
    std::uint64_t sequence;

    auto operator<=>(const StreamId&) const = default;

    [[nodiscard]] std::string to_string() const {
        return std::to_string(milliseconds) + "-" + std::to_string(sequence);
    }
};

struct StreamEntry {
    StreamId id;
    std::vector<std::pair<std::string, std::string>> values;
};

struct IdRequest {
    enum class Mode {
        AUTOMATIC,
        AUTOMATIC_SEQUENCE,
        EXPLICIT,
    };

    Mode mode;
    std::uint64_t milliseconds{};
    std::uint64_t sequence{};
};

enum class ReadMode {
    IMMEDIATE,
    BLOCK_INDEFINITELY,
    BLOCK_WITH_TIMEOUT,
};

struct ReadOptions {
    ReadMode mode;
    std::chrono::milliseconds timeout{};
};

enum class ReadStartMode {
    AFTER_ID,
    LATEST,
};

struct ReadRequest {
    std::string key;
    ReadStartMode start_mode;
    StreamId after_id{};
};

struct ReadResult {
    std::string key;
    std::vector<StreamEntry> entries;
};

} // namespace rstream
