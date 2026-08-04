#include "command_processor.hpp"
#include "commands.hpp"
#include "resp.hpp"
#include "rlist.hpp"
#include "rstream.hpp"
#include "server_config.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Handler = resp::Response (*)(Database&, const resp::Command&);

enum class CommandAccess { READ_ONLY, WRITE };

struct CommandDefinition {
    std::string_view command;
    Handler handler;
    CommandAccess access;
};

struct WaitRequest {
    std::size_t required_replicas;
    std::optional<std::chrono::milliseconds> timeout;
};

constexpr std::array command_handlers{
    CommandDefinition{
        .command = "PING",
        .handler = [](Database&, const resp::Command& command) { return commands::ping(command); },
        .access = CommandAccess::READ_ONLY,
    },
    CommandDefinition{
        .command = "ECHO",
        .handler = [](Database&, const resp::Command& command) { return commands::echo(command); },
        .access = CommandAccess::READ_ONLY,
    },
    CommandDefinition{
        .command = "SET",
        .handler = commands::set,
        .access = CommandAccess::WRITE,
    },
    CommandDefinition{
        .command = "GET",
        .handler = commands::get,
        .access = CommandAccess::READ_ONLY,
    },
    CommandDefinition{
        .command = "TYPE",
        .handler = commands::type,
        .access = CommandAccess::READ_ONLY,
    },
    CommandDefinition{
        .command = "INCR",
        .handler = commands::incr,
        .access = CommandAccess::WRITE,
    },
    CommandDefinition{
        .command = "RPUSH",
        .handler = rlist::rpush,
        .access = CommandAccess::WRITE,
    },
    CommandDefinition{
        .command = "LPUSH",
        .handler = rlist::lpush,
        .access = CommandAccess::WRITE,
    },
    CommandDefinition{
        .command = "LRANGE",
        .handler = rlist::lrange,
        .access = CommandAccess::READ_ONLY,
    },
    CommandDefinition{
        .command = "LLEN",
        .handler = rlist::llen,
        .access = CommandAccess::READ_ONLY,
    },
    CommandDefinition{
        .command = "LPOP",
        .handler = rlist::lpop,
        .access = CommandAccess::WRITE,
    },
    CommandDefinition{
        .command = "BLPOP",
        .handler = rlist::blpop,
        .access = CommandAccess::WRITE,
    },
    CommandDefinition{
        .command = "XADD",
        .handler = rstream::xadd,
        .access = CommandAccess::WRITE,
    },
    CommandDefinition{
        .command = "XRANGE",
        .handler = rstream::xrange,
        .access = CommandAccess::READ_ONLY,
    },
    CommandDefinition{
        .command = "XREAD",
        .handler = rstream::xread,
        .access = CommandAccess::READ_ONLY,
    },
};

[[nodiscard]] const CommandDefinition*
find_command_definition(std::string_view command_name) {
    const auto definition =
        std::ranges::find(command_handlers, command_name, &CommandDefinition::command);

    if (definition == command_handlers.end()) {
        return nullptr;
    }

    return &*definition;
}

[[nodiscard]] bool is_write_command(const resp::Command& command) {
    if (command.empty()) {
        return false;
    }

    const auto* definition = find_command_definition(command.front());
    return definition != nullptr && definition->access == CommandAccess::WRITE;
}

[[nodiscard]] bool response_is_error(const resp::Response& response) {
    const auto& response_variant = static_cast<const resp::ResponseVariant&>(response);
    return std::holds_alternative<resp::SimpleError>(response_variant);
}

[[nodiscard]] bool write_modified_database(const resp::Command& command,
                                           const resp::Response& response) {
    const auto& response_variant = static_cast<const resp::ResponseVariant&>(response);

    if (response_is_error(response)) {
        return false;
    }

    if (command.front() == "BLPOP") {
        const auto* values = std::get_if<resp::Array>(&response_variant);
        return values != nullptr && !values->values.empty();
    }

    if (command.front() != "LPOP") {
        return true;
    }

    if (std::holds_alternative<resp::NullBulkString>(response_variant)) {
        return false;
    }

    const auto* values = std::get_if<resp::Array>(&response_variant);
    return values == nullptr || !values->values.empty();
}

void normalize_replication_command(resp::Command& command, const resp::Response& response) {
    const auto& response_variant = static_cast<const resp::ResponseVariant&>(response);

    if (command.front() == "XADD") {
        if (const auto* generated_id = std::get_if<resp::BulkString>(&response_variant)) {
            command[2] = generated_id->value;
        }
    }

    if (command.front() == "BLPOP") {
        command = {"LPOP", command[1]};
    }
}

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned_integer(std::string_view text) {
    std::uint64_t value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);

    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }

    return value;
}

[[nodiscard]] std::expected<WaitRequest, resp::SimpleError>
parse_wait_request(const resp::Command& command) {
    if (command.size() != 3) {
        return std::unexpected(resp::SimpleError{
            .value = "ERR wrong number of arguments for 'wait' command",
        });
    }

    auto required_replicas = parse_unsigned_integer(command[1]);
    auto timeout = parse_unsigned_integer(command[2]);

    if (!required_replicas || !timeout ||
        *required_replicas > std::numeric_limits<std::size_t>::max() ||
        *timeout > static_cast<std::uint64_t>(
                       std::numeric_limits<std::chrono::milliseconds::rep>::max())) {
        return std::unexpected(resp::SimpleError{
            .value = "ERR value is not an integer or out of range",
        });
    }

    std::optional<std::chrono::milliseconds> parsed_timeout;

    if (*timeout != 0) {
        parsed_timeout = std::chrono::milliseconds{
            static_cast<std::chrono::milliseconds::rep>(*timeout)};
    }

    return WaitRequest{
        .required_replicas = static_cast<std::size_t>(*required_replicas),
        .timeout = parsed_timeout,
    };
}

[[nodiscard]] resp::Response dispatch(Database& database, const resp::Command& command) {
    if (command.empty()) {
        return resp::NullBulkString{};
    }

    const auto* definition = find_command_definition(command.front());

    if (definition == nullptr) {
        return resp::SimpleError{
            .value = "ERR unknown command",
        };
    }

    return definition->handler(database, command);
}

std::string generate_replication_id() {
    constexpr char hexadecimal[] = "0123456789abcdef";
    constexpr std::size_t replication_id_length = 40;

    std::random_device random;
    std::uniform_int_distribution<int> distribution{0, 15};

    std::string result(replication_id_length, '\0');

    for (char& character : result) {
        character = hexadecimal[distribution(random)];
    }

    return result;
}

} // namespace

CommandProcessor::CommandProcessor(const std::optional<ReplicaConfig>& replica_of)
    : _replication_state(ReplicationState{.role = replica_of.has_value() ? ReplicationRole::REPLICA
                                                                         : ReplicationRole::MASTER,
                                          .replication_id = generate_replication_id(),
                                          .offset = 0}),
      _worker([this](std::stop_token stop_token) { run(stop_token); }) {}

CommandProcessor::~CommandProcessor() {
    _worker.request_stop();
    _task_available.notify_all();

    if (_worker.joinable()) {
        _worker.join();
    }
}

std::string_view CommandProcessor::get_replication_role() const {
    if (_replication_state.role == ReplicationRole::MASTER) {
        return "master";
    }
    return "slave";
}

std::future<resp::Response> CommandProcessor::submit(std::uint64_t client_id,
                                                     resp::Command command) {
    return enqueue(client_id, std::move(command), CommandSource::CLIENT, 0);
}

std::future<resp::Response>
CommandProcessor::apply_replication(resp::Command command, std::size_t bytes_consumed) {
    constexpr std::uint64_t replication_client_id = 0;
    return enqueue(replication_client_id, std::move(command), CommandSource::MASTER,
                   bytes_consumed);
}

std::future<resp::Response> CommandProcessor::enqueue(std::uint64_t client_id,
                                                      resp::Command command,
                                                      CommandSource source,
                                                      std::size_t replication_bytes) {
    Task task{.client_id = client_id,
              .command = std::move(command),
              .source = source,
              .replication_bytes = replication_bytes,
              .response = {}};

    std::future<resp::Response> future = task.response.get_future();

    {
        std::lock_guard lock(_task_mutex);
        _tasks.emplace_back(std::move(task));
    }

    _task_available.notify_one();

    return future;
}

std::future<void>
CommandProcessor::install_replication_state(std::string replication_id,
                                            std::uint64_t offset) {
    InstallReplicationStateTask task{
        .replication_id = std::move(replication_id),
        .offset = offset,
        .completion = {},
    };
    std::future<void> future = task.completion.get_future();

    {
        std::lock_guard lock(_task_mutex);
        _tasks.emplace_back(std::move(task));
    }

    _task_available.notify_one();

    return future;
}

std::future<std::uint64_t> CommandProcessor::replication_offset() {
    GetReplicationOffsetTask task{.offset = {}};
    std::future<std::uint64_t> future = task.offset.get_future();

    {
        std::lock_guard lock(_task_mutex);
        _tasks.emplace_back(std::move(task));
    }

    _task_available.notify_one();

    return future;
}

std::future<void>
CommandProcessor::acknowledge_replica(std::uint64_t client_id, std::uint64_t offset) {
    AcknowledgeReplicaTask task{
        .client_id = client_id,
        .offset = offset,
        .completion = {},
    };
    std::future<void> future = task.completion.get_future();

    {
        std::lock_guard lock(_task_mutex);
        _tasks.emplace_back(std::move(task));
    }

    _task_available.notify_one();

    return future;
}

std::future<ReplicaRegistration>
CommandProcessor::register_replica(std::uint64_t client_id) {
    RegisterReplicaTask task{.client_id = client_id, .registration = {}};
    std::future<ReplicaRegistration> future = task.registration.get_future();

    {
        std::lock_guard lock(_task_mutex);
        _tasks.emplace_back(std::move(task));
    }

    _task_available.notify_one();

    return future;
}

resp::Response CommandProcessor::process_command(std::uint64_t client_id, resp::Command& command,
                                                 CommandSource source) {
    if (command.empty()) {
        return resp::SimpleError{
            .value = "ERR empty command",
        };
    }

    const auto& cmd_name = command.front();

    if (cmd_name == "INFO") {
        return resp::BulkString{
            .value = std::format("# Replication\r\n"
                                 "role:{}\r\n"
                                 "master_replid:{}\r\n"
                                 "master_repl_offset:{}\r\n",
                                 get_replication_role(), _replication_state.replication_id,
                                 _replication_state.offset),
        };
    }

    if (cmd_name == "REPLCONF") {
        if (command.size() != 3) {
            return resp::SimpleError{
                .value = "ERR wrong number of arguments for 'replconf' command",
            };
        }

        return resp::SimpleString{.value = "OK"};
    }

    if (cmd_name == "PSYNC") {
        if (command.size() != 3) {
            return resp::SimpleError{
                .value = "ERR wrong number of arguments for 'psync' command",
            };
        }

        return resp::SimpleString{
            .value = std::format("FULLRESYNC {} {}", _replication_state.replication_id,
                                 _replication_state.offset),
        };
    }

    if (cmd_name == "WATCH") {
        if (command.size() < 2) {
            return resp::SimpleError{
                .value = "ERR Invalid syntax. Expected usage: WATCH <key1> <key2> ..."};
        }

        if (_transactions.contains(client_id)) {
            return resp::SimpleError{
                .value = "ERR WATCH inside MULTI is not allowed",
            };
        }

        auto& watched = _watched_keys[client_id];

        for (std::size_t i = 1; i < command.size(); ++i) {
            const auto& key = command[i];

            if (!watched.contains(key)) {
                watched.emplace(key, _database.key_revision(key));
            }
        }

        return resp::SimpleString{.value = "OK"};
    }

    if (cmd_name == "UNWATCH") {
        if (command.size() != 1) {
            return resp::SimpleError{
                .value = "ERR wrong number of arguments for 'unwatch' command",
            };
        }

        _watched_keys.erase(client_id);

        return resp::SimpleString{.value = "OK"};
    }

    if (cmd_name == "MULTI") {
        const auto [transaction, inserted] = _transactions.try_emplace(client_id);

        if (!inserted) {
            return resp::SimpleError{.value = "ERR MULTI calls cannot be nested"};
        }

        return resp::SimpleString{.value = "OK"};
    }

    if (cmd_name == "EXEC") {
        return execute_transaction(client_id, source);
    }

    if (cmd_name == "DISCARD") {
        const std::size_t removed = _transactions.erase(client_id);

        if (removed == 0) {
            return resp::SimpleError{.value = "ERR DISCARD without MULTI"};
        }

        _watched_keys.erase(client_id);

        return resp::SimpleString{.value = "OK"};
    }

    auto transaction = _transactions.find(client_id);

    if (transaction != _transactions.end()) {
        transaction->second.commands.push_back(std::move(command));

        return resp::SimpleString{.value = "QUEUED"};
    }

    return dispatch(_database, command);
}

void CommandProcessor::append_replication_frame(std::string payload) {
    _replication_state.offset += static_cast<std::uint64_t>(payload.size());

    auto frame = std::make_shared<const ReplicationFrame>(ReplicationFrame{
        .payload = std::move(payload),
        .ending_offset = _replication_state.offset,
    });

    _replication_backlog.push_back(frame);

    std::erase_if(_replica_sessions, [&frame](const auto& weak_session) {
        auto session = weak_session.lock();

        if (!session) {
            return true;
        }

        session->enqueue(frame);
        return false;
    });
}

void CommandProcessor::append_client_replication_frame(std::uint64_t client_id,
                                                       std::string payload) {
    append_replication_frame(std::move(payload));
    _client_write_offsets[client_id] = _replication_state.offset;
}

void CommandProcessor::append_blocking_pop_frame(const Task& task, const std::string& key) {
    if (_replication_state.role != ReplicationRole::MASTER ||
        task.source != CommandSource::CLIENT) {
        return;
    }

    append_client_replication_frame(task.client_id,
                                    resp::serialize_command({"LPOP", key}));
}

resp::Response CommandProcessor::execute_transaction(std::uint64_t client_id,
                                                     CommandSource source) {
    auto transaction = _transactions.find(client_id);

    if (transaction == _transactions.end()) {
        return resp::SimpleError{.value = "ERR EXEC without MULTI"};
    }

    bool watched_key_changed = false;

    const auto watched = _watched_keys.find(client_id);

    if (watched != _watched_keys.end()) {
        for (const auto& [key, original_revision] : watched->second) {
            if (_database.key_revision(key) != original_revision) {
                watched_key_changed = true;
                break;
            }
        }
    }

    const bool transaction_has_error = transaction->second.has_error;
    std::vector<resp::Command> commands = std::move(transaction->second.commands);
    _transactions.erase(transaction);

    _watched_keys.erase(client_id);

    if (transaction_has_error) {
        return resp::SimpleError{
            .value = "EXECABORT Transaction discarded because of previous errors.",
        };
    }

    if (watched_key_changed) {
        return resp::NullArray{};
    }

    if (commands.empty()) {
        return resp::EmptyArray{};
    }

    std::vector<resp::Response> responses;
    responses.reserve(commands.size());

    const bool replicate_transaction =
        _replication_state.role == ReplicationRole::MASTER && source == CommandSource::CLIENT;

    std::string replication_payload;

    for (auto& command : commands) {
        resp::Response response = dispatch(_database, command);

        if (replicate_transaction && is_write_command(command) &&
            write_modified_database(command, response)) {
            if (replication_payload.empty()) {
                replication_payload = resp::serialize_command({"MULTI"});
            }

            normalize_replication_command(command, response);
            replication_payload += resp::serialize_command(command);
        }

        responses.push_back(std::move(response));
    }

    if (!replication_payload.empty()) {
        replication_payload += resp::serialize_command({"EXEC"});
        append_client_replication_frame(client_id, std::move(replication_payload));
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
        append_blocking_pop_frame(task, request->key);

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
    const bool reject_replica_write =
        _replication_state.role == ReplicationRole::REPLICA &&
        task.source == CommandSource::CLIENT && is_write_command(task.command);

    if (reject_replica_write) {
        auto transaction = _transactions.find(task.client_id);

        if (transaction != _transactions.end()) {
            transaction->second.has_error = true;
        }

        task.response.set_value(resp::SimpleError{
            .value = "READONLY You can't write against a read only replica.",
        });
        return;
    }

    const bool is_blpop = !task.command.empty() && task.command.front() == "BLPOP";
    const bool is_xread = !task.command.empty() && task.command.front() == "XREAD";
    const bool is_wait = !task.command.empty() && task.command.front() == "WAIT";
    const bool transaction_is_active = _transactions.contains(task.client_id);

    if (is_blpop && !transaction_is_active) {
        process_blpop(std::move(task));
        return;
    }

    if (is_xread && !transaction_is_active) {
        process_xread(std::move(task));
        return;
    }

    if (is_wait && !transaction_is_active) {
        process_wait(std::move(task));
        return;
    }

    const bool replicate_direct_write =
        _replication_state.role == ReplicationRole::MASTER &&
        task.source == CommandSource::CLIENT && !transaction_is_active &&
        is_write_command(task.command);

    resp::Response response = process_command(task.client_id, task.command, task.source);

    if (replicate_direct_write && write_modified_database(task.command, response)) {
        normalize_replication_command(task.command, response);
        append_client_replication_frame(task.client_id,
                                        resp::serialize_command(task.command));
    }

    if (task.source == CommandSource::MASTER && !response_is_error(response)) {
        _replication_state.offset +=
            static_cast<std::uint64_t>(task.replication_bytes);
    }

    task.response.set_value(std::move(response));
}

void CommandProcessor::process_replica_registration(RegisterReplicaTask task) {
    auto session =
        std::make_shared<ReplicaSession>(task.client_id, _replication_state.offset);

    _replica_sessions.push_back(session);

    task.registration.set_value(ReplicaRegistration{
        .replication_id = _replication_state.replication_id,
        .starting_offset = _replication_state.offset,
        .session = std::move(session),
    });
}

void CommandProcessor::process_replication_state_installation(
    InstallReplicationStateTask task) {
    _replication_state.replication_id = std::move(task.replication_id);
    _replication_state.offset = task.offset;
    task.completion.set_value();
}

void CommandProcessor::process_replication_offset_request(GetReplicationOffsetTask task) {
    task.offset.set_value(_replication_state.offset);
}

void CommandProcessor::process_replica_acknowledgement(AcknowledgeReplicaTask task) {
    auto& acknowledged_offset = _replica_acknowledged_offsets[task.client_id];
    acknowledged_offset = std::max(acknowledged_offset, task.offset);
    task.completion.set_value();
}

std::size_t CommandProcessor::count_acknowledged_replicas(std::uint64_t target_offset) {
    std::size_t count = 0;

    std::erase_if(_replica_sessions,
                  [this, target_offset, &count](const auto& weak_session) {
                      auto session = weak_session.lock();

                      if (!session) {
                          return true;
                      }

                      const auto acknowledged =
                          _replica_acknowledged_offsets.find(session->client_id());

                      if (acknowledged != _replica_acknowledged_offsets.end() &&
                          acknowledged->second >= target_offset) {
                          ++count;
                      }

                      return false;
                  });

    return count;
}

void CommandProcessor::process_wait(Task task) {
    if (_replication_state.role != ReplicationRole::MASTER) {
        task.response.set_value(resp::SimpleError{
            .value = "ERR WAIT cannot be used with replica instances",
        });
        return;
    }

    auto request = parse_wait_request(task.command);

    if (!request) {
        task.response.set_value(request.error());
        return;
    }

    std::uint64_t target_offset = 0;
    const auto client_offset = _client_write_offsets.find(task.client_id);

    if (client_offset != _client_write_offsets.end()) {
        target_offset = client_offset->second;
    }

    const std::size_t acknowledged = count_acknowledged_replicas(target_offset);

    if (acknowledged >= request->required_replicas) {
        task.response.set_value(resp::Integer{
            .value = static_cast<std::int64_t>(acknowledged),
        });
        return;
    }

    append_replication_frame(
        resp::serialize_command({"REPLCONF", "GETACK", "*"}));

    std::optional<std::chrono::steady_clock::time_point> deadline;

    if (request->timeout) {
        deadline = std::chrono::steady_clock::now() + *request->timeout;
    }

    _pending_waits.push_back(PendingWait{
        .task = std::move(task),
        .required_replicas = request->required_replicas,
        .target_offset = target_offset,
        .deadline = deadline,
    });
}

void CommandProcessor::retry_pending_list_pops() {
    auto pending = _pending_list_pops.begin();

    while (pending != _pending_list_pops.end()) {
        auto result = _database.pop_list_element(pending->key);

        if (result) {
            append_blocking_pop_frame(pending->task, pending->key);

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
                    .value = "WRONGTYPE Operation against a key holding the wrong kind of value"});
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

void CommandProcessor::retry_pending_waits() {
    auto pending = _pending_waits.begin();

    while (pending != _pending_waits.end()) {
        const std::size_t acknowledged =
            count_acknowledged_replicas(pending->target_offset);

        if (acknowledged < pending->required_replicas) {
            ++pending;
            continue;
        }

        pending->task.response.set_value(resp::Integer{
            .value = static_cast<std::int64_t>(acknowledged),
        });
        pending = _pending_waits.erase(pending);
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

void CommandProcessor::expire_pending_waits() {
    const auto now = std::chrono::steady_clock::now();
    auto pending = _pending_waits.begin();

    while (pending != _pending_waits.end()) {
        if (!pending->deadline || now < *pending->deadline) {
            ++pending;
            continue;
        }

        const std::size_t acknowledged =
            count_acknowledged_replicas(pending->target_offset);

        pending->task.response.set_value(resp::Integer{
            .value = static_cast<std::int64_t>(acknowledged),
        });
        pending = _pending_waits.erase(pending);
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

    for (const auto& pending : _pending_waits) {
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
        std::optional<ProcessorTask> task;

        {
            std::unique_lock lock(_task_mutex);

            auto task_is_available = [&] { return stop_token.stop_requested() || !_tasks.empty(); };

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
            expire_pending_waits();
            continue;
        }

        if (auto* command_task = std::get_if<Task>(&*task)) {
            process_task(std::move(*command_task));
        } else if (auto* registration_task = std::get_if<RegisterReplicaTask>(&*task)) {
            process_replica_registration(std::move(*registration_task));
        } else if (auto* installation_task =
                       std::get_if<InstallReplicationStateTask>(&*task)) {
            process_replication_state_installation(
                std::move(*installation_task));
        } else if (auto* offset_task = std::get_if<GetReplicationOffsetTask>(&*task)) {
            process_replication_offset_request(std::move(*offset_task));
        } else {
            process_replica_acknowledgement(
                std::move(std::get<AcknowledgeReplicaTask>(*task)));
        }

        retry_pending_list_pops();
        retry_pending_stream_reads();
        retry_pending_waits();
        expire_pending_list_pops();
        expire_pending_stream_reads();
        expire_pending_waits();
    }
}
