#include "command_executor.hpp"
#include "database.hpp"
#include "resp.hpp"
#include "rlist.hpp"
#include "utils/time.hpp"
#include <chrono>
#include <optional>
#include <utility>

resp::Response CommandExecutor::execute(const resp::Command& command) {
    if (command.empty()) {
        return resp::NullBulkString{};
    }

    const auto& cmd = command[0];

    if (cmd == "PING") {
        return resp::SimpleString{
            .value = "PONG",
        };
    }

    if (cmd == "ECHO") {

        if (command.size() != 2) {
            return resp::SimpleError{.value = "ERR expected format ECHO <text>"};
        }

        return resp::BulkString{.value = command[1]};
    }

    if (cmd == "SET") {
        if (command.size() == 3) {
            _database.set(command[1], command[2], std::nullopt);
            return resp::SimpleString{.value = "OK"};
        }
        if (command.size() == 5) {

            auto parsed_expiration = time_utils::parse_expiry(command[4]);

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

    if (cmd == "GET") {
        if (command.size() != 2) {
            return resp::SimpleError{.value = "ERR invalid syntax. Expected usage GET <key>"};
        }

        auto value = _database.get(command[1]);

        if (!value) {
            if (value.error() == Database::Error::WRONG_TYPE) {
                return resp::SimpleError{
                    .value =
                        "WRONGTYPE Operation against a key holding the wrong kind of value"};
            }

            return resp::NullBulkString{};
        }

        return resp::BulkString{.value = std::move(*value)};
    }

    if (cmd == "TYPE") {
        if (command.size() != 2) {
            return resp::SimpleError{
                .value = "ERR invalid syntax. Expected usage TYPE <key>"};
        }

        switch (_database.get_type(command[1])) {
        case Database::ValueType::NONE:
            return resp::SimpleString{.value = "none"};

        case Database::ValueType::STRING:
            return resp::SimpleString{.value = "string"};

        case Database::ValueType::LIST:
            return resp::SimpleString{.value = "list"};
        }

        return resp::SimpleError{.value = "ERR unsupported value type"};
    }

    if (cmd == "RPUSH") {
        return rlist::rpush(_database, command);
    }

    if (cmd == "LPUSH") {
        return rlist::lpush(_database, command);
    }

    if (cmd == "LRANGE") {
        return rlist::lrange(_database, command);
    }

    if (cmd == "LLEN") {
        return rlist::llen(_database, command);
    }

    if (cmd == "LPOP") {
        return rlist::lpop(_database, command);
    }

    if (cmd == "BLPOP") {
        return rlist::blpop(_database, command);
    }

    return resp::SimpleError{.value = "ERR unknown command"};
}
