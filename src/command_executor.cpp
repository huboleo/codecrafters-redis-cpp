#include "command_executor.hpp"
#include "resp.hpp"

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

    return resp::SimpleError{.value = "ERR unknown command"};
}
