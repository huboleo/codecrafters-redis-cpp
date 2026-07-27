#include "resp.hpp"
#include <charconv>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace {
std::optional<std::string_view> read_line(std::string_view input, std::size_t& position) {
    const std::size_t end = input.find("\r\n", position);

    if (end == std::string::npos) {
        return std::nullopt;
    }

    std::string_view line = input.substr(position, end - position);
    position = end + 2;

    return line;
}

std::expected<std::size_t, resp::ParseError> parse_length(std::string_view text) {
    std::size_t length = 0;

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), length);

    if (text.empty() or error != std::errc{} or end != text.data() + text.size()) {
        return std::unexpected(resp::ParseError{.code = resp::ParseErrorCode::INVALID_LENGTH,
                                                .message = "Invalid RESP length"});
    }

    return length;
}

std::string serialize_line(char prefix, std::string value) {
    value.reserve(value.size() + 3);
    value.insert(value.begin(), prefix);
    value += "\r\n";

    return value;
}
} // namespace

resp::ParseOutcome resp::parse_command(std::string_view input) {
    if (input.empty()) {
        return Incomplete{};
    }

    if (input.front() != '*') {
        return ParseError{
            .code = ParseErrorCode::INVALID_FORMAT,
            .message = "Expected RESP array",
        };
    }

    std::size_t position = 1;

    auto array_length_text = read_line(input, position);

    if (!array_length_text) {
        return Incomplete{};
    }

    auto array_length = parse_length(*array_length_text);

    if (!array_length) {
        return array_length.error();
    }

    Command command;

    for (std::size_t i = 0; i < *array_length; ++i) {
        if (position >= input.size()) {
            return Incomplete{};
        }

        if (input[position] != '$') {
            return ParseError{
                .code = ParseErrorCode::INVALID_FORMAT,
                .message = "Expected RESP bulk string",
            };
        }

        ++position;
        auto bulk_length_text = read_line(input, position);

        if (!bulk_length_text) {
            return Incomplete{};
        }

        auto bulk_length = parse_length(*bulk_length_text);

        if (!bulk_length) {
            return bulk_length.error();
        }

        if (*bulk_length > input.size() - position) {
            return Incomplete{};
        }

        std::string_view argument = input.substr(position, *bulk_length);

        position += *bulk_length;

        if (input.size() - position < 2) {
            return Incomplete{};
        }

        if (input[position] != '\r' or input[position + 1] != '\n') {
            return ParseError{.code = ParseErrorCode::INVALID_FORMAT,
                              .message = "Bulk string must end with CRLF"};
        }

        position += 2;
        command.emplace_back(argument);
    }

    return ParseResult{
        .command = std::move(command),
        .bytes_consumed = position,
    };
}

std::string resp::serialize_response(Response response) {
    if (auto* simple_string = std::get_if<SimpleString>(&response)) {
        std::string result = std::move(simple_string->value);

        return serialize_line('+', std::move(result));
    }

    if (auto* error = std::get_if<SimpleError>(&response)) {
        std::string result = std::move(error->value);

        return serialize_line('-', std::move(result));
    }

    if (auto* bulk_string = std::get_if<BulkString>(&response)) {
        std::string result = std::move(bulk_string->value);

        std::string prefix = "$" + std::to_string(result.size()) + "\r\n";

        result.reserve(prefix.size() + result.size() + 2);
        result.insert(0, prefix);
        result += "\r\n";

        return result;
    }

    if (auto* integer = std::get_if<Integer>(&response)) {
        return ":" + std::to_string(integer->value) + "\r\n";
    }

    if (std::holds_alternative<Null>(response)) {
        return "$-1\r\n";
    }

    if (std::holds_alternative<EmptyArray>(response)) {
        return "*0\r\n";
    }

    if (auto* array = std::get_if<Array>(&response)) {
        std::string result = "*" + std::to_string(array->values.size()) + "\r\n";
        for (const auto& value : array->values) {
            result += "$" + std::to_string(value.size()) + "\r\n";
            result += value;
            result += "\r\n";
        }
        return result;
    }

    return {};
}
