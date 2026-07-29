#include "rstream.hpp"
#include "database.hpp"
#include "optional"
#include "resp.hpp"
#include "rstream_types.hpp"
#include <charconv>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace {
std::optional<std::uint64_t> parse_text_as_number(std::string_view text) {
    std::uint64_t number{};

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || number < 0) {
        return std::nullopt;
    }

    return number;
}

std::optional<std::vector<std::pair<std::string, std::string>>>
parse_value_list(std::span<const std::string> values) {
    if (values.empty() || values.size() % 2 != 0) {
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(values.size() / 2);

    for (std::size_t i = 0; i < values.size(); i += 2) {
        result.emplace_back(values[i], values[i + 1]);
    }

    return result;
}

std::optional<Database::StreamId> parse_range_start(std::string_view start) {
    if (start == "-") {
        return Database::StreamId{
            .milliseconds = 0,
            .sequence = 0,
        };
    }

    Database::StreamId id{};

    auto dash_position = start.find_first_of('-');

    if (dash_position == std::string::npos) {
        auto milliseconds = parse_text_as_number(start);
        if (!milliseconds) {
            return std::nullopt;
        }
        id.milliseconds = *milliseconds;
        id.sequence = 0;
    } else {
        auto milliseconds = parse_text_as_number(start.substr(0, dash_position));
        auto sequence = parse_text_as_number(start.substr(dash_position + 1));

        if (!milliseconds || !sequence) {
            return std::nullopt;
        }

        id.milliseconds = *milliseconds;
        id.sequence = *sequence;
    }

    return id;
}
std::optional<Database::StreamId> parse_range_stop(std::string_view stop) {
    if (stop == "+") {
        return Database::StreamId{
            .milliseconds = std::numeric_limits<std::uint64_t>::max(),
            .sequence = std::numeric_limits<std::uint64_t>::max(),
        };
    }

    Database::StreamId id{};

    auto dash_position = stop.find_first_of('-');

    if (dash_position == std::string::npos) {
        auto milliseconds = parse_text_as_number(stop);
        if (!milliseconds) {
            return std::nullopt;
        }
        id.milliseconds = *milliseconds;
        id.sequence = std::numeric_limits<std::uint64_t>::max();
    } else {
        auto milliseconds = parse_text_as_number(stop.substr(0, dash_position));
        auto sequence = parse_text_as_number(stop.substr(dash_position + 1));

        if (!milliseconds || !sequence) {
            return std::nullopt;
        }

        id.milliseconds = *milliseconds;
        id.sequence = *sequence;
    }

    return id;
}

} // namespace

std::optional<rstream::IdRequest> rstream::parse_id(std::string_view id) {
    if (id == "*") {
        return IdRequest{.mode = IdRequest::Mode::AUTOMATIC};
    }

    auto dash_position = id.find_first_of('-');

    if (dash_position == std::string::npos) {
        return std::nullopt;
    }

    auto milliseconds_part_text = id.substr(0, dash_position);
    auto sequence_part_text = id.substr(dash_position + 1);

    auto parsed_milliseconds_part = parse_text_as_number(milliseconds_part_text);

    if (!parsed_milliseconds_part) {
        return std::nullopt;
    }

    if (sequence_part_text == "*") {
        return IdRequest{.mode = IdRequest::Mode::AUTOMATIC_SEQUENCE,
                         .milliseconds = *parsed_milliseconds_part};
    }

    auto parsed_sequence_part = parse_text_as_number(sequence_part_text);

    if (!parsed_sequence_part) {
        return std::nullopt;
    }

    return IdRequest{.mode = IdRequest::Mode::EXPLICIT,
                     .milliseconds = *parsed_milliseconds_part,
                     .sequence = *parsed_sequence_part};
}

resp::Response rstream::xadd(Database& database, const resp::Command& command) {
    if (command.size() < 5) {
        return resp::SimpleError{
            .value = "ERR Invalid usage. Expected usage <XADD> <name> <id> <key> <value> pairs..."};
    }

    auto parsed_id = parse_id(command[2]);

    if (!parsed_id) {
        return resp::SimpleError{.value = "ERR Invalid id format"};
    }

    if (parsed_id->mode == IdRequest::Mode::EXPLICIT && parsed_id->milliseconds == 0 &&
        parsed_id->sequence == 0) {
        return resp::SimpleError{.value = "ERR The ID specified in XADD must be greater than 0-0"};
    }

    auto parsed_values = parse_value_list(std::span<const std::string>{command}.subspan(3));

    if (!parsed_values) {
        return resp::SimpleError{.value = "ERR Invalid value list format"};
    }

    auto result = database.add_stream(command[1], *parsed_id, std::move(*parsed_values));

    if (!result) {
        if (result.error() == Database::Error::WRONG_TYPE) {
            return resp::SimpleError{
                .value = "WRONGTYPE Operation against a key holding the wrong kind of value"};
        }

        if (result.error() == Database::Error::INVALID_STREAM_ID_VALUE) {
            return resp::SimpleError{.value = "ERR The ID specified in XADD is equal or smaller "
                                              "than the target stream top item"};
        }
    }

    return resp::BulkString{.value = result->to_string()};
}

resp::Response rstream::xrange(Database& database, const resp::Command& command) {
    if (command.size() != 4) {
        return resp::SimpleError{
            .value = "ERR Invalid usage. Expected usage <XRANGE> <name> <start> <stop>"};
    }

    auto start = parse_range_start(command[2]);
    auto stop = parse_range_stop(command[3]);

    if (!start || !stop) {
        return resp::SimpleError{.value = "ERR Invalid range indexes format"};
    }

    auto result = database.stream_range(command[1], *start, *stop);

    if (!result) {
        if (result.error() == Database::Error::KEY_NOT_FOUND) {
            return resp::SimpleError{.value = "ERR stream with given key does not exits"};
        }

        if (result.error() == Database::Error::WRONG_TYPE) {
            return resp::SimpleError{.value = "ERR wrong type"};
        }
    }

    std::vector<resp::ArrayElement> entries;
    entries.reserve(result->size());

    for (auto& entry : *result) {
        std::vector<resp::ArrayElement> values;
        values.reserve(entry.values.size() * 2);

        for (auto& [field, value] : entry.values) {
            values.push_back(resp::ArrayElement{.value = std::move(field)});
            values.push_back(resp::ArrayElement{.value = std::move(value)});
        }

        std::vector<resp::ArrayElement> entry_values;
        entry_values.reserve(2);
        entry_values.push_back(resp::ArrayElement{.value = entry.id.to_string()});
        entry_values.push_back(resp::ArrayElement{.value = std::move(values)});

        entries.push_back(resp::ArrayElement{.value = std::move(entry_values)});
    }

    return resp::NestedArray{.values = std::move(entries)};
}
