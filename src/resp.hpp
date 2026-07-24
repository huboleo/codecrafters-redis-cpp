#pragma once
#include <string>
#include <utility>

namespace resp {
enum class RespDataType {
    SIMPLE_STRING,
    SIMPLE_ERROR,
    INTEGER,
    BULK_STRING,
    NULL_BULK_STRING,
    ARRAY,
    NULL_TYPE,
    BOOLEAN,
    DOUBLE,
    BIG_NUMBER,
    BULK_ERROR,
    VERBATIM_STRING,
    MAP,
    ATTRIBUTE,
    SET,
    PUSH
};

std::string serialize_string_response(RespDataType type, std::string response);
} // namespace resp
