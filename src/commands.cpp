#include "commands.hpp"
#include "database.hpp"
#include "resp.hpp"
#include "utils/time.hpp"

resp::Response commands::ping(const resp::Command& command) {
    return resp::SimpleString{
        .value = "PONG",
    };
}

resp::Response commands::echo(const resp::Command& command) {
    if (command.size() != 2) {
        return resp::SimpleError{.value = "ERR expected format ECHO <text>"};
    }

    return resp::BulkString{.value = command[1]};
}

resp::Response commands::set(Database& database, const resp::Command& command) {
    if (command.size() == 3) {
        database.set(command[1], command[2], std::nullopt);
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

        database.set(command[1], command[2], expiry);
        return resp::SimpleString{.value = "OK"};
    }

    return resp::SimpleError{
        .value = "ERR invalid syntax. Expected usage SET <key> <value> <EX/PX> <time>"};
}

resp::Response commands::get(Database& database, const resp::Command& command) {
    if (command.size() != 2) {
        return resp::SimpleError{.value = "ERR invalid syntax. Expected usage GET <key>"};
    }

    auto value = database.get(command[1]);

    if (!value) {
        if (value.error() == Database::Error::WRONG_TYPE) {
            return resp::SimpleError{
                .value = "WRONGTYPE Operation against a key holding the wrong kind of value"};
        }

        return resp::NullBulkString{};
    }

    return resp::BulkString{.value = std::move(*value)};
}

resp::Response commands::keys(Database& database, const resp::Command& command) {
    if (command.size() != 2) {
        return resp::SimpleError{.value = "ERR wrong number of arguments for 'keys' command"};
    }

    if (command[1] != "*") {
        return resp::SimpleError{.value = "ERR only the '*' key pattern is supported"};
    }

    return resp::Array{.values = database.keys()};
}

resp::Response commands::type(Database& database, const resp::Command& command) {
    if (command.size() != 2) {
        return resp::SimpleError{.value = "ERR invalid syntax. Expected usage TYPE <key>"};
    }

    switch (database.get_type(command[1])) {
    case Database::ValueType::NONE:
        return resp::SimpleString{.value = "none"};

    case Database::ValueType::STRING:
        return resp::SimpleString{.value = "string"};

    case Database::ValueType::LIST:
        return resp::SimpleString{.value = "list"};

    case Database::ValueType::STREAM:
        return resp::SimpleString{.value = "stream"};
    }
    return resp::SimpleError{.value = "ERR unsupported value type"};
}

resp::Response commands::incr(Database& database, const resp::Command& command) {
    if (command.size() != 2) {
        return resp::SimpleError{.value = "ERR invalid syntax. Expected usage INCR <key>"};
    }

    auto result = database.increment_key(command[1]);

    if (!result) {
        if (result.error() == Database::Error::WRONG_TYPE) {
            return resp::SimpleError{
                .value = "WRONGTYPE Operation against a key holding the wrong kind of value"};
        }

        if (result.error() == Database::Error::VALUE_NOT_INTEGER) {
            return resp::SimpleError{.value = "ERR value is not an integer or out of range"};
        }
    }

    return resp::Integer{.value = *result};
}
