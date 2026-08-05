#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace resp {

// Parsing
using Command = std::vector<std::string>;
struct ParseResult {
    Command command;
    std::size_t bytes_consumed;
};

enum class ParseErrorCode {
    INVALID_FORMAT,
    INVALID_LENGTH,
};

struct ParseError {
    ParseErrorCode code;
    std::string message;
};

struct Incomplete {};

using ParseOutcome = std::variant<ParseResult, Incomplete, ParseError>;

[[nodiscard]] ParseOutcome parse_command(std::string_view input);

[[nodiscard]] std::string serialize_command(const Command& command);

// Serializing Response

struct SimpleString {
    std::string value;
};
struct SimpleError {
    std::string value;
};
struct BulkString {
    std::string value;
};
struct Integer {
    std::int64_t value;
};
struct NullBulkString {};
struct NullArray {};
struct EmptyArray {};
struct NoResponse {};
struct Array {
    std::vector<std::string> values;
};

struct Response;

struct ResponseArray {
    std::vector<Response> values;
};

using ResponseVariant = std::variant<SimpleString, SimpleError, BulkString, Integer,
                                     NullBulkString, NullArray, EmptyArray, NoResponse, Array,
                                     ResponseArray>;

struct Response : ResponseVariant {
    using ResponseVariant::ResponseVariant;
    using ResponseVariant::operator=;
};

std::string serialize_response(Response response);

} // namespace resp
