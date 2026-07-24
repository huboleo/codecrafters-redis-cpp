#include "resp.hpp"

#include <format>
#include <string>
#include <utility>

using resp::RespDataType;

std::string resp::serialize_string_response(RespDataType type, std::string response) {
    switch (type) {
    case RespDataType::SIMPLE_STRING:
        response.insert(0, "+");
        response += "\r\n";
        return response;
    }

    return {};
}
