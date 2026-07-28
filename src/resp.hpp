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
struct Array {
    std::vector<std::string> values;
};

using Response = std::variant<SimpleString, SimpleError, BulkString, Integer, NullBulkString,
                              NullArray, EmptyArray, Array>;

std::string serialize_response(Response response);

} // namespace resp
