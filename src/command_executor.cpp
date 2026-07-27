#include "command_executor.hpp"
#include "database.hpp"
#include "resp.hpp"
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <system_error>

namespace {
std::optional<std::chrono::milliseconds::rep> get_expiration_count(const std::string& text) {
    std::chrono::milliseconds::rep count{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);

    if (error != std::errc{} or end != text.data() + text.size() or count <= 0) {
        return std::nullopt;
    }

    return count;
}

std::optional<std::int64_t> get_range_index(const std::string& text) {
    std::int64_t index{};

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), index);

    if (error != std::errc{} or end != text.data() + text.size()) {
        return std::nullopt;
    }

    return index;
}
} // namespace

resp::Response CommandExecutor::execute(const resp::Command& command) {
    if (command.empty()) {
        return resp::Null{};
    }

    if (command[0] == "PING") {
        return resp::SimpleString{
            .value = "PONG",
        };
    }

    if (command[0] == "ECHO") {

        if (command.size() != 2) {
            return resp::SimpleError{.value = "ERR expected format ECHO <text>"};
        }

        return resp::BulkString{.value = command[1]};
    }

    if (command[0] == "SET") {
        if (command.size() == 3) {
            _database.set(command[1], command[2], std::nullopt);
            return resp::SimpleString{.value = "OK"};
        }
        if (command.size() == 5) {

            auto parsed_expiration = get_expiration_count(command[4]);

            if (!parsed_expiration) {
                return resp::SimpleError{.value = "ERR invalid expire time in set command"};
            }

            std::chrono::milliseconds expiry;

            if (command[3] == "PX") {
                expiry = std::chrono::milliseconds{*parsed_expiration};
            } else if (command[3] == "EX") {
                expiry = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::seconds{*parsed_expiration});
            } else {
                return resp::SimpleError{.value = "ERR syntax error"};
            }

            _database.set(command[1], command[2], expiry);
            return resp::SimpleString{.value = "OK"};
        }

        return resp::SimpleError{
            .value = "ERR invalid syntax. Expected usage SET <key> <value> <EX/PX> <time>"};
    }

    if (command[0] == "GET") {
        if (command.size() != 2) {
            return resp::SimpleError{.value = "ERR invalid syntax. Expected usage GET <key>"};
        }

        auto value = _database.get(command[1]);

        if (!value) {
            return resp::Null{};
        }

        return resp::BulkString{.value = std::move(*value)};
    }

    if (command[0] == "RPUSH") {
        if (command.size() < 3) {
            return resp::SimpleError{
                .value = "ERR invalid syntax. Expected usage RPUSH <list_name> <values...>"};
        }
        std::vector<std::string> values(command.begin() + 2, command.end());
        auto size = _database.add_list_elements(command[1], std::move(values),
                                                Database::ListAddMode::APPEND);
        return resp::Integer{.value = static_cast<int64_t>(size)};
    }

    if (command[0] == "LPUSH") {
        if (command.size() < 3) {
            return resp::SimpleError{
                .value = "ERR invalid syntax. Expected usage LPUSH <list_name> <values...>"};
        }
        std::vector<std::string> values(command.begin() + 2, command.end());
        auto size = _database.add_list_elements(command[1], std::move(values),
                                                Database::ListAddMode::PREPEND);
        return resp::Integer{.value = static_cast<int64_t>(size)};
    }

    if (command[0] == "LRANGE") {
        if (command.size() != 4) {
            return resp::SimpleError{
                .value = "ERR invalid syntax. Expected usage LRANGE <list_name> <start_index> "
                         "<stop_index>"};
        }

        auto start_index = get_range_index(command[2]);
        auto stop_index = get_range_index(command[3]);

        if (!start_index or !stop_index) {
            return resp::SimpleError{.value = "ERR invalid index value"};
        }

        auto values = _database.list_elements(command[1], *start_index, *stop_index);

        if (!values) {
            return resp::EmptyArray{};
        }

        return resp::Array{.values = std::move(*values)};
    }

    return resp::SimpleError{.value = "ERR unknown command"};
}
