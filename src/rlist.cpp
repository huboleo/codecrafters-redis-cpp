#include "rlist.hpp"

#include "database.hpp"
#include "resp.hpp"
#include "utils/time.hpp"
#include <charconv>
#include <chrono>
#include <expected>
#include <string>
#include <vector>

namespace {
std::optional<std::int64_t> get_range_index(const std::string& text) {
    std::int64_t index{};

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), index);

    if (error != std::errc{} or end != text.data() + text.size()) {
        return std::nullopt;
    }

    return index;
}

std::expected<int, resp::SimpleError> parse_number_of_elements(std::string_view text) {
    int count{};

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);

    if (text.empty() or error != std::errc{} or end != text.data() + text.size() or count <= 0) {
        return std::unexpected(resp::SimpleError{.value = "ERR Invalid number of items format"});
    }

    return count;
}

} // namespace

resp::Response rlist::rpush(Database& database, const resp::Command& command) {
    if (command.size() < 3) {
        return resp::SimpleError{
            .value = "ERR invalid syntax. Expected usage RPUSH <list_name> <values...>"};
    }
    std::vector<std::string> values(command.begin() + 2, command.end());
    auto size =
        database.add_list_elements(command[1], std::move(values), Database::ListAddMode::APPEND);
    return resp::Integer{.value = static_cast<int64_t>(size)};
}

resp::Response rlist::lpush(Database& database, const resp::Command& command) {
    if (command.size() < 3) {
        return resp::SimpleError{
            .value = "ERR invalid syntax. Expected usage LPUSH <list_name> <values...>"};
    }
    std::vector<std::string> values(command.begin() + 2, command.end());
    auto size =
        database.add_list_elements(command[1], std::move(values), Database::ListAddMode::PREPEND);
    return resp::Integer{.value = static_cast<int64_t>(size)};
}

resp::Response rlist::lrange(Database& database, const resp::Command& command) {
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

    auto values = database.list_elements(command[1], *start_index, *stop_index);

    if (!values) {
        return resp::EmptyArray{};
    }

    return resp::Array{.values = std::move(*values)};
}

resp::Response rlist::llen(Database& database, const resp::Command& command) {
    if (command.size() != 2) {
        return resp::SimpleError{.value = "ERR invalid syntax. Expected usage LLEN <list_name>"};
    }

    auto size = database.get_list_length(command[1]);
    return resp::Integer{.value = static_cast<std::int64_t>(size)};
}

resp::Response rlist::lpop(Database& database, const resp::Command& command) {
    if (command.size() != 2 and command.size() != 3) {
        return resp::SimpleError{
            .value = "ERR invalid syntax. Expected usage LPOP <list_name> <number_of_items?>"};
    }

    if (command.size() == 2) {
        auto result = database.pop_list_element(command[1]);

        if (!result) {
            return resp::NullBulkString{};
        }

        return resp::BulkString{.value = *result};
    }

    auto parsed_number = parse_number_of_elements(command[2]);

    if (!parsed_number) {
        return parsed_number.error();
    }

    auto result = database.pop_list_elements(command[1], *parsed_number);

    if (!result) {
        return resp::NullBulkString{};
    }

    return resp::Array{.values = *result};
}

resp::Response rlist::blpop(Database& database, const resp::Command& command) {
    if (command.size() != 3) {
        return resp::SimpleError{
            .value = "ERR invalid syntax. Expected usage BLPOP <list_name> <timeout>"};
    }

    auto parsed_timeout = time_utils::parse_blocking_timeout(command[2]);

    if (!parsed_timeout) {
        return resp::SimpleError{.value = "ERR Invalid timeout value"};
    }

    std::optional<Database::Milliseconds> timeout;

    if (*parsed_timeout > 0) {
        const auto fractional_seconds = std::chrono::duration<double>{*parsed_timeout};

        timeout = std::chrono::duration_cast<Database::Milliseconds>(fractional_seconds);
    }

    auto result = database.pop_list_element_blocking(command[1], timeout);

    if (!result) {
        return resp::NullArray{};
    }

    return resp::Array{.values = {command[1], std::move(*result)}};
}
