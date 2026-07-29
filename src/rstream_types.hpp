#pragma once
#include <cstdint>
namespace rstream {

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

} // namespace rstream
