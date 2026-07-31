#include "command_processor.hpp"
#include "commands.hpp"
#include "resp.hpp"
#include "rlist.hpp"
#include "rstream.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Handler = resp::Response (*)(Database&, const resp::Command&);

struct CommandDefinition {
    std::string_view command;
    Handler handler;
};

constexpr std::array command_handlers{
    CommandDefinition{
        .command = "PING",
        .handler = [](Database&, const resp::Command& command) { return commands::ping(command); },
    },
    CommandDefinition{
        .command = "ECHO",
        .handler = [](Database&, const resp::Command& command) { return commands::echo(command); },
    },
    CommandDefinition{.command = "SET", .handler = commands::set},
    CommandDefinition{.command = "GET", .handler = commands::get},
    CommandDefinition{.command = "TYPE", .handler = commands::type},
    CommandDefinition{.command = "INCR", .handler = commands::incr},
    CommandDefinition{.command = "RPUSH", .handler = rlist::rpush},
    CommandDefinition{.command = "LPUSH", .handler = rlist::lpush},
    CommandDefinition{.command = "LRANGE", .handler = rlist::lrange},
    CommandDefinition{.command = "LLEN", .handler = rlist::llen},
    CommandDefinition{.command = "LPOP", .handler = rlist::lpop},
    CommandDefinition{.command = "BLPOP", .handler = rlist::blpop},
    CommandDefinition{.command = "XADD", .handler = rstream::xadd},
    CommandDefinition{.command = "XRANGE", .handler = rstream::xrange},
    CommandDefinition{.command = "XREAD", .handler = rstream::xread},
};

[[nodiscard]] resp::Response dispatch(Database& database, const resp::Command& command) {
    if (command.empty()) {
        return resp::NullBulkString{};
    }

    const auto definition =
        std::ranges::find(command_handlers, command.front(), &CommandDefinition::command);

    if (definition == command_handlers.end()) {
        return resp::SimpleError{
            .value = "ERR unknown command",
        };
    }

    return definition->handler(database, command);
}

} // namespace

CommandProcessor::CommandProcessor()
    : _worker([this](std::stop_token stop_token) { run(stop_token); }) {}

CommandProcessor::~CommandProcessor() {
    _worker.request_stop();
    _task_available.notify_all();

    if (_worker.joinable()) {
        _worker.join();
    }
}

std::future<resp::Response> CommandProcessor::submit(std::uint64_t client_id,
                                                     resp::Command command) {
    Task task{.client_id = client_id, .command = std::move(command), .response = {}};

    std::future<resp::Response> future = task.response.get_future();

    {
        std::lock_guard lock(_task_mutex);
        _tasks.push_back(std::move(task));
    }

    _task_available.notify_one();

    return future;
}

resp::Response CommandProcessor::process_command(std::uint64_t client_id, resp::Command command) {
    if (command.empty()) {
        return resp::SimpleError{
            .value = "ERR empty command",
        };
    }

    const auto& cmd_name = command.front();

    if (cmd_name == "MULTI") {
        const auto [transaction, inserted] = _transactions.try_emplace(client_id);

        if (!inserted) {
            return resp::SimpleError{.value = "ERR MULTI calls cannot be nested"};
        }

        return resp::SimpleString{.value = "OK"};
    }

    if (cmd_name == "EXEC") {
        return execute_transaction(client_id);
    }

    if (cmd_name == "DISCARD") {
        const std::size_t removed = _transactions.erase(client_id);

        if (removed == 0) {
            return resp::SimpleError{.value = "ERR DISCARD without MULTI"};
        }

        return resp::SimpleString{.value = "OK"};
    }

    auto transaction = _transactions.find(client_id);

    if (transaction != _transactions.end()) {
        transaction->second.push_back(std::move(command));

        return resp::SimpleString{.value = "QUEUED"};
    }

    return dispatch(_database, command);
}

resp::Response CommandProcessor::execute_transaction(std::uint64_t client_id) {
    auto transaction = _transactions.find(client_id);

    if (transaction == _transactions.end()) {
        return resp::SimpleError{.value = "ERR EXEC without MULTI"};
    }

    std::vector<resp::Command> commands = std::move(transaction->second);
    _transactions.erase(transaction);

    if (commands.empty()) {
        return resp::EmptyArray{};
    }

    std::vector<resp::Response> responses;
    responses.reserve(commands.size());

    for (const auto& command : commands) {
        responses.push_back(dispatch(_database, command));
    }

    return resp::ResponseArray{.values = std::move(responses)};
}

void CommandProcessor::process_blpop(Task task) {
    auto request = rlist::parse_blpop(task.command);

    if (!request) {
        task.response.set_value(request.error());
        return;
    }

    auto result = _database.pop_list_element(request->key);

    if (result) {
        std::vector<std::string> values;
        values.reserve(2);
        values.push_back(std::move(request->key));
        values.push_back(std::move(*result));

        task.response.set_value(resp::Array{.values = std::move(values)});
        return;
    }

    if (result.error() == Database::Error::WRONG_TYPE) {
        task.response.set_value(resp::SimpleError{
            .value = "WRONGTYPE Operation against a key holding the wrong kind of value"});
        return;
    }

    std::optional<std::chrono::steady_clock::time_point> deadline;

    if (request->timeout) {
        deadline = std::chrono::steady_clock::now() + *request->timeout;
    }

    _pending_list_pops.push_back(PendingListPop{
        .task = std::move(task),
        .key = std::move(request->key),
        .deadline = deadline,
    });
}

void CommandProcessor::process_xread(Task task) {
    auto read_command = rstream::parse_xread(task.command);

    if (!read_command) {
        task.response.set_value(read_command.error());
        return;
    }

    auto result = _database.read_streams(read_command->requests);

    if (!result) {
        if (result.error() == Database::Error::WRONG_TYPE) {
            task.response.set_value(resp::SimpleError{
                .value = "WRONGTYPE Operation against a key holding the wrong kind of value"});
        } else {
            task.response.set_value(resp::SimpleError{.value = "ERR Unable to read streams"});
        }

        return;
    }

    if (!result->empty()) {
        task.response.set_value(rstream::make_xread_response(std::move(*result)));
        return;
    }

    if (read_command->options.mode == rstream::ReadMode::IMMEDIATE) {
        task.response.set_value(resp::NullArray{});
        return;
    }

    std::optional<std::chrono::steady_clock::time_point> deadline;

    if (read_command->options.mode == rstream::ReadMode::BLOCK_WITH_TIMEOUT) {
        deadline = std::chrono::steady_clock::now() + read_command->options.timeout;
    }

    _pending_stream_reads.push_back(PendingStreamRead{
        .task = std::move(task),
        .requests = std::move(read_command->requests),
        .deadline = deadline,
    });
}

void CommandProcessor::process_task(Task task) {
    const bool is_blpop = !task.command.empty() && task.command.front() == "BLPOP";
    const bool is_xread = !task.command.empty() && task.command.front() == "XREAD";
    const bool transaction_is_active = _transactions.contains(task.client_id);

    if (is_blpop && !transaction_is_active) {
        process_blpop(std::move(task));
        return;
    }

    if (is_xread && !transaction_is_active) {
        process_xread(std::move(task));
        return;
    }

    resp::Response response =
        process_command(task.client_id, std::move(task.command));

    task.response.set_value(std::move(response));
}

void CommandProcessor::retry_pending_list_pops() {
    auto pending = _pending_list_pops.begin();

    while (pending != _pending_list_pops.end()) {
        auto result = _database.pop_list_element(pending->key);

        if (result) {
            std::vector<std::string> values;
            values.reserve(2);
            values.push_back(std::move(pending->key));
            values.push_back(std::move(*result));

            pending->task.response.set_value(resp::Array{.values = std::move(values)});
            pending = _pending_list_pops.erase(pending);
            continue;
        }

        if (result.error() == Database::Error::WRONG_TYPE) {
            pending->task.response.set_value(resp::SimpleError{
                .value = "WRONGTYPE Operation against a key holding the wrong kind of value"});
            pending = _pending_list_pops.erase(pending);
            continue;
        }

        ++pending;
    }
}

void CommandProcessor::retry_pending_stream_reads() {
    auto pending = _pending_stream_reads.begin();

    while (pending != _pending_stream_reads.end()) {
        auto result = _database.read_streams(pending->requests);

        if (!result) {
            if (result.error() == Database::Error::WRONG_TYPE) {
                pending->task.response.set_value(resp::SimpleError{
                    .value =
                        "WRONGTYPE Operation against a key holding the wrong kind of value"});
            } else {
                pending->task.response.set_value(
                    resp::SimpleError{.value = "ERR Unable to read streams"});
            }

            pending = _pending_stream_reads.erase(pending);
            continue;
        }

        if (result->empty()) {
            ++pending;
            continue;
        }

        pending->task.response.set_value(rstream::make_xread_response(std::move(*result)));
        pending = _pending_stream_reads.erase(pending);
    }
}

void CommandProcessor::expire_pending_list_pops() {
    const auto now = std::chrono::steady_clock::now();
    auto pending = _pending_list_pops.begin();

    while (pending != _pending_list_pops.end()) {
        if (!pending->deadline || now < *pending->deadline) {
            ++pending;
            continue;
        }

        pending->task.response.set_value(resp::NullArray{});
        pending = _pending_list_pops.erase(pending);
    }
}

void CommandProcessor::expire_pending_stream_reads() {
    const auto now = std::chrono::steady_clock::now();
    auto pending = _pending_stream_reads.begin();

    while (pending != _pending_stream_reads.end()) {
        if (!pending->deadline || now < *pending->deadline) {
            ++pending;
            continue;
        }

        pending->task.response.set_value(resp::NullArray{});
        pending = _pending_stream_reads.erase(pending);
    }
}

std::optional<std::chrono::steady_clock::time_point>
CommandProcessor::next_pending_deadline() const {
    std::optional<std::chrono::steady_clock::time_point> next_deadline;

    for (const auto& pending : _pending_list_pops) {
        if (!pending.deadline) {
            continue;
        }

        if (!next_deadline || *pending.deadline < *next_deadline) {
            next_deadline = pending.deadline;
        }
    }

    for (const auto& pending : _pending_stream_reads) {
        if (!pending.deadline) {
            continue;
        }

        if (!next_deadline || *pending.deadline < *next_deadline) {
            next_deadline = pending.deadline;
        }
    }

    return next_deadline;
}

void CommandProcessor::run(std::stop_token stop_token) {
    while (true) {
        std::optional<Task> task;

        {
            std::unique_lock lock(_task_mutex);

            auto task_is_available = [&] {
                return stop_token.stop_requested() || !_tasks.empty();
            };

            auto deadline = next_pending_deadline();

            if (deadline) {
                _task_available.wait_until(lock, *deadline, task_is_available);
            } else {
                _task_available.wait(lock, task_is_available);
            }

            if (stop_token.stop_requested() && _tasks.empty()) {
                return;
            }

            if (!_tasks.empty()) {
                task.emplace(std::move(_tasks.front()));
                _tasks.pop_front();
            }
        }

        if (!task) {
            expire_pending_list_pops();
            expire_pending_stream_reads();
            continue;
        }

        process_task(std::move(*task));
        retry_pending_list_pops();
        retry_pending_stream_reads();
        expire_pending_list_pops();
        expire_pending_stream_reads();
    }
}
