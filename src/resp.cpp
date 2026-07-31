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

    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(resp::ParseError{.code = resp::ParseErrorCode::INVALID_LENGTH,
                                                .message = "Invalid RESP length"});
    }

    return length;
}

void append_bulk_string(std::string& output, const std::string& value) {
    output += "$";
    output += std::to_string(value.size());
    output += "\r\n";
    output += value;
    output += "\r\n";
}

void append_response(std::string& output, const resp::Response& response);

void append_value(std::string& output, const resp::SimpleString& value) {
    output += "+";
    output += value.value;
    output += "\r\n";
}

void append_value(std::string& output, const resp::SimpleError& value) {
    output += "-";
    output += value.value;
    output += "\r\n";
}

void append_value(std::string& output, const resp::BulkString& value) {
    append_bulk_string(output, value.value);
}

void append_value(std::string& output, const resp::Integer& value) {
    output += ":";
    output += std::to_string(value.value);
    output += "\r\n";
}

void append_value(std::string& output, const resp::NullBulkString&) {
    output += "$-1\r\n";
}

void append_value(std::string& output, const resp::NullArray&) {
    output += "*-1\r\n";
}

void append_value(std::string& output, const resp::EmptyArray&) {
    output += "*0\r\n";
}

void append_value(std::string& output, const resp::Array& array) {
    output += "*";
    output += std::to_string(array.values.size());
    output += "\r\n";

    for (const auto& value : array.values) {
        append_bulk_string(output, value);
    }
}

void append_value(std::string& output, const resp::ResponseArray& array) {
    output += "*";
    output += std::to_string(array.values.size());
    output += "\r\n";

    for (const auto& response : array.values) {
        append_response(output, response);
    }
}

void append_response(std::string& output, const resp::Response& response) {
    std::visit([&output](const auto& value) { append_value(output, value); },
               static_cast<const resp::ResponseVariant&>(response));
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

        if (input[position] != '\r' || input[position + 1] != '\n') {
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
    std::string result;

    append_response(result, response);

    return result;
}
