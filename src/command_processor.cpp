#include "command_processor.hpp"
#include "commands.hpp"
#include "resp.hpp"
#include "rlist.hpp"
#include "rstream.hpp"
#include "server_config.hpp"
#include "utils/sha256.hpp"
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
        .command = "KEYS",
        .handler = commands::keys,
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

[[nodiscard]] const CommandDefinition* find_command_definition(std::string_view command_name) {
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
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

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
        parsed_timeout =
            std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(*timeout)};
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

acl::User make_default_acl_user(const std::optional<std::string>& password_hash) {
    acl::User user{
        .enabled = true,
        .no_password = !password_hash.has_value(),
        .all_commands = true,
        .all_keys = true,
        .all_channels = true,
    };

    if (password_hash) {
        user.password_hashes.insert(*password_hash);
    }

    return user;
}

} // namespace

CommandProcessor::CommandProcessor(const std::optional<ReplicaConfig>& replica_of,
                                   const RDBConfig& rdb_config, const AOFConfig& aof_config,
                                   const std::optional<std::string>& default_user_password_hash)
    : _acl_users{{"default", make_default_acl_user(default_user_password_hash)}},
      _rdb_config(rdb_config), _aof_config(aof_config),
      _replication_state(ReplicationState{.role = replica_of.has_value() ? ReplicationRole::REPLICA
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

std::future<resp::Response> CommandProcessor::apply_replication(resp::Command command,
                                                                std::size_t bytes_consumed) {
    constexpr std::uint64_t replication_client_id = 0;
    return enqueue(replication_client_id, std::move(command), CommandSource::MASTER,
                   bytes_consumed);
}

std::future<resp::Response> CommandProcessor::enqueue(std::uint64_t client_id,
                                                      resp::Command command, CommandSource source,
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

std::future<void> CommandProcessor::install_replication_state(std::string replication_id,
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

std::future<void> CommandProcessor::acknowledge_replica(std::uint64_t client_id,
                                                        std::uint64_t offset) {
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

std::future<ReplicaRegistration> CommandProcessor::register_replica(std::uint64_t client_id) {
    RegisterReplicaTask task{.client_id = client_id, .registration = {}};
    std::future<ReplicaRegistration> future = task.registration.get_future();

    {
        std::lock_guard lock(_task_mutex);
        _tasks.emplace_back(std::move(task));
    }

    _task_available.notify_one();

    return future;
}

std::future<void>
CommandProcessor::load_rdb_entries(std::vector<rdb::StringEntry> entries) {
    LoadRdbTask task{
        .entries = std::move(entries),
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

std::future<std::expected<void, std::string>>
CommandProcessor::initialize_aof(aof::AppendOnlyFile file,
                                 std::vector<resp::Command> commands) {
    InitializeAofTask task{
        .file = std::move(file),
        .commands = std::move(commands),
        .completion = {},
    };
    std::future<std::expected<void, std::string>> future = task.completion.get_future();

    {
        std::lock_guard lock(_task_mutex);
        _tasks.emplace_back(std::move(task));
    }

    _task_available.notify_one();
    return future;
}

std::future<void>
CommandProcessor::register_client(std::uint64_t client_id,
                                  std::shared_ptr<PubSubSession> pubsub_session) {
    RegisterClientTask task{
        .client_id = client_id,
        .pubsub_session = std::move(pubsub_session),
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

std::future<void> CommandProcessor::unregister_client(std::uint64_t client_id) {
    UnregisterClientTask task{
        .client_id = client_id,
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

resp::Response CommandProcessor::process_command(std::uint64_t client_id, resp::Command& command,
                                                 CommandSource source) {
    if (command.empty()) {
        return resp::SimpleError{
            .value = "ERR empty command",
        };
    }

    const auto& cmd_name = command.front();

    if (cmd_name == "SUBSCRIBE") {
        return subscribe(client_id, command);
    }

    if (cmd_name == "UNSUBSCRIBE") {
        return unsubscribe(client_id, command);
    }

    if (_client_subscriptions.contains(client_id) && cmd_name == "PING") {
        if (command.size() > 2) {
            return resp::SimpleError{
                .value = "ERR wrong number of arguments for 'ping' command",
            };
        }

        return resp::Array{
            .values = {"pong", command.size() == 2 ? command[1] : ""},
        };
    }

    if (cmd_name == "PUBLISH") {
        return publish(command);
    }

    if (cmd_name == "AUTH") {
        return authenticate(client_id, command);
    }

    if (cmd_name == "ACL") {
        return process_acl_command(client_id, command);
    }

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

    if (cmd_name == "CONFIG") {
        if (command.size() != 3 || command[1] != "GET") {
            return resp::SimpleError{
                .value = "ERR syntax error",
            };
        }

        if (command[2] == "dir") {
            return resp::Array{
                .values = {"dir", _rdb_config.dir},
            };
        }

        if (command[2] == "dbfilename") {
            return resp::Array{
                .values = {"dbfilename", _rdb_config.db_filename},
            };
        }

        if (command[2] == "appendonly") {
            return resp::Array{
                .values = {"appendonly", _aof_config.enabled ? "yes" : "no"},
            };
        }

        if (command[2] == "appenddirname") {
            return resp::Array{
                .values = {"appenddirname", _aof_config.append_dirname},
            };
        }

        if (command[2] == "appendfilename") {
            return resp::Array{
                .values = {"appendfilename", _aof_config.append_filename},
            };
        }

        if (command[2] == "appendfsync") {
            return resp::Array{
                .values = {"appendfsync", _aof_config.append_fsync},
            };
        }

        return resp::EmptyArray{};
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

std::expected<void, std::string>
CommandProcessor::persist_blocking_pop(const Task& task, const std::string& key) {
    const std::string payload = resp::serialize_command({"LPOP", key});

    if (_aof_file && task.source != CommandSource::AOF) {
        auto result = _aof_file->append(payload);

        if (!result) {
            return std::unexpected(result.error());
        }
    }

    if (_replication_state.role == ReplicationRole::MASTER &&
        task.source == CommandSource::CLIENT) {
        append_client_replication_frame(task.client_id, payload);
    }

    return {};
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
    const bool persist_transaction = _aof_file.has_value() && source != CommandSource::AOF;

    std::string persistence_payload;

    for (auto& command : commands) {
        resp::Response response = dispatch(_database, command);

        if ((replicate_transaction || persist_transaction) && is_write_command(command) &&
            write_modified_database(command, response)) {
            if (persistence_payload.empty()) {
                persistence_payload = resp::serialize_command({"MULTI"});
            }

            normalize_replication_command(command, response);
            persistence_payload += resp::serialize_command(command);
        }

        responses.push_back(std::move(response));
    }

    if (!persistence_payload.empty()) {
        persistence_payload += resp::serialize_command({"EXEC"});

        if (persist_transaction) {
            auto result = _aof_file->append(persistence_payload);

            if (!result) {
                return resp::SimpleError{
                    .value = "MISCONF AOF persistence error: " + result.error(),
                };
            }
        }

        if (replicate_transaction) {
            append_client_replication_frame(client_id, std::move(persistence_payload));
        }
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
        auto persistence = persist_blocking_pop(task, request->key);

        if (!persistence) {
            task.response.set_value(resp::SimpleError{
                .value = "MISCONF AOF persistence error: " + persistence.error(),
            });
            return;
        }

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
    const bool is_auth_command =
        !task.command.empty() && task.command.front() == "AUTH";

    if (task.source == CommandSource::CLIENT && !is_auth_command) {
        auto client = _clients.find(task.client_id);
        bool authenticated = client != _clients.end() &&
                             client->second.authenticated_username.has_value();

        if (authenticated) {
            const auto user = _acl_users.find(*client->second.authenticated_username);
            authenticated = user != _acl_users.end() && user->second.enabled;

            if (!authenticated) {
                client->second.authenticated_username.reset();
            }
        }

        if (!authenticated) {
            resp::Response response = resp::SimpleError{
                .value = "NOAUTH Authentication required.",
            };

            if (_client_subscriptions.contains(task.client_id)) {
                auto enqueue_result =
                    enqueue_pubsub_response(task.client_id, std::move(response));

                if (enqueue_result) {
                    response = resp::NoResponse{};
                } else {
                    response = resp::SimpleError{
                        .value = "ERR failed to queue Pub/Sub response: " +
                                 enqueue_result.error(),
                    };
                }
            }

            task.response.set_value(std::move(response));
            return;
        }
    }

    const bool client_is_subscribed =
        task.source == CommandSource::CLIENT && _client_subscriptions.contains(task.client_id);
    const bool command_is_allowed_while_subscribed =
        !task.command.empty() &&
        (task.command.front() == "SUBSCRIBE" || task.command.front() == "UNSUBSCRIBE" ||
         task.command.front() == "PING");

    if (client_is_subscribed && !command_is_allowed_while_subscribed) {
        resp::Response response = resp::SimpleError{
            .value = std::format(
                "ERR Can't execute '{}': only SUBSCRIBE, UNSUBSCRIBE and PING are allowed in "
                "this context",
                task.command.empty() ? "" : task.command.front()),
        };
        auto enqueue_result = enqueue_pubsub_response(task.client_id, std::move(response));

        if (enqueue_result) {
            task.response.set_value(resp::NoResponse{});
        } else {
            task.response.set_value(resp::SimpleError{
                .value = "ERR failed to queue Pub/Sub response: " + enqueue_result.error(),
            });
        }

        return;
    }

    const bool reject_replica_write = _replication_state.role == ReplicationRole::REPLICA &&
                                      task.source == CommandSource::CLIENT &&
                                      is_write_command(task.command);

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

    const bool write_command = !transaction_is_active && is_write_command(task.command);
    const bool replicate_direct_write = _replication_state.role == ReplicationRole::MASTER &&
                                        task.source == CommandSource::CLIENT && write_command;
    const bool persist_direct_write =
        _aof_file.has_value() && task.source != CommandSource::AOF && write_command;

    resp::Response response = process_command(task.client_id, task.command, task.source);
    const bool command_succeeded = !response_is_error(response);

    if ((replicate_direct_write || persist_direct_write) &&
        write_modified_database(task.command, response)) {
        normalize_replication_command(task.command, response);
        std::string payload = resp::serialize_command(task.command);

        if (persist_direct_write) {
            auto result = _aof_file->append(payload);

            if (!result) {
                response = resp::SimpleError{
                    .value = "MISCONF AOF persistence error: " + result.error(),
                };
            }
        }

        if (replicate_direct_write && !response_is_error(response)) {
            append_client_replication_frame(task.client_id, std::move(payload));
        }
    }

    if (task.source == CommandSource::MASTER && command_succeeded) {
        _replication_state.offset += static_cast<std::uint64_t>(task.replication_bytes);
    }

    const bool response_already_queued =
        std::holds_alternative<resp::NoResponse>(
            static_cast<const resp::ResponseVariant&>(response));

    if (task.source == CommandSource::CLIENT &&
        _client_subscriptions.contains(task.client_id) && !response_already_queued) {
        auto enqueue_result = enqueue_pubsub_response(task.client_id, std::move(response));

        if (enqueue_result) {
            response = resp::NoResponse{};
        } else {
            response = resp::SimpleError{
                .value = "ERR failed to queue Pub/Sub response: " + enqueue_result.error(),
            };
        }
    }

    task.response.set_value(std::move(response));
}

void CommandProcessor::process_replica_registration(RegisterReplicaTask task) {
    auto session = std::make_shared<ReplicaSession>(task.client_id, _replication_state.offset);

    _replica_sessions.push_back(session);

    task.registration.set_value(ReplicaRegistration{
        .replication_id = _replication_state.replication_id,
        .starting_offset = _replication_state.offset,
        .session = std::move(session),
    });
}

void CommandProcessor::process_replication_state_installation(InstallReplicationStateTask task) {
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

void CommandProcessor::process_rdb_load(LoadRdbTask task) {
    const auto unix_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();

    for (auto& entry : task.entries) {
        std::optional<std::chrono::milliseconds> expiry;

        if (entry.expires_at_unix_ms) {
            if (unix_now < 0 ||
                *entry.expires_at_unix_ms <= static_cast<std::uint64_t>(unix_now)) {
                continue;
            }

            const std::uint64_t remaining =
                *entry.expires_at_unix_ms - static_cast<std::uint64_t>(unix_now);
            const std::uint64_t maximum = static_cast<std::uint64_t>(
                std::numeric_limits<std::chrono::milliseconds::rep>::max());

            expiry = std::chrono::milliseconds{
                static_cast<std::chrono::milliseconds::rep>(std::min(remaining, maximum))};
        }

        _database.set(std::move(entry.key), std::move(entry.value), expiry);
    }

    task.completion.set_value();
}

void CommandProcessor::process_aof_initialization(InitializeAofTask task) {
    constexpr std::uint64_t aof_client_id = std::numeric_limits<std::uint64_t>::max();

    for (auto& command : task.commands) {
        resp::Response response = process_command(aof_client_id, command, CommandSource::AOF);
        const auto& response_variant = static_cast<const resp::ResponseVariant&>(response);

        if (const auto* error = std::get_if<resp::SimpleError>(&response_variant)) {
            _transactions.erase(aof_client_id);
            _watched_keys.erase(aof_client_id);
            task.completion.set_value(std::unexpected(
                "Failed to replay AOF command: " + error->value));
            return;
        }
    }

    if (_transactions.erase(aof_client_id) != 0) {
        _watched_keys.erase(aof_client_id);
        task.completion.set_value(
            std::unexpected("AOF file ended before its transaction was completed"));
        return;
    }

    _watched_keys.erase(aof_client_id);
    _aof_file.emplace(std::move(task.file));
    task.completion.set_value(std::expected<void, std::string>{});
}

void CommandProcessor::process_client_registration(RegisterClientTask task) {
    std::optional<std::string> authenticated_username;
    const auto default_user = _acl_users.find("default");

    if (default_user != _acl_users.end() && default_user->second.enabled &&
        default_user->second.no_password) {
        authenticated_username = "default";
    }

    _clients.insert_or_assign(task.client_id,
                              ClientState{
                                  .authenticated_username = std::move(authenticated_username),
                                  .pubsub_session = std::move(task.pubsub_session),
                              });
    task.completion.set_value();
}

void CommandProcessor::process_client_unregistration(UnregisterClientTask task) {
    auto subscriptions = _client_subscriptions.find(task.client_id);

    if (subscriptions != _client_subscriptions.end()) {
        for (const auto& channel : subscriptions->second) {
            auto subscribers = _channel_subscribers.find(channel);

            if (subscribers == _channel_subscribers.end()) {
                continue;
            }

            subscribers->second.erase(task.client_id);

            if (subscribers->second.empty()) {
                _channel_subscribers.erase(subscribers);
            }
        }

        _client_subscriptions.erase(subscriptions);
    }

    _clients.erase(task.client_id);
    task.completion.set_value();
}

resp::Response CommandProcessor::subscribe(std::uint64_t client_id,
                                           const resp::Command& command) {
    if (command.size() < 2) {
        return resp::SimpleError{
            .value = "ERR wrong number of arguments for 'subscribe' command",
        };
    }

    const auto client = _clients.find(client_id);

    if (client == _clients.end()) {
        return resp::SimpleError{
            .value = "ERR Pub/Sub session is not registered",
        };
    }

    auto session = client->second.pubsub_session.lock();

    if (!session) {
        _clients.erase(client);
        return resp::SimpleError{
            .value = "ERR Pub/Sub session is no longer available",
        };
    }

    auto& subscriptions = _client_subscriptions[client_id];
    std::string acknowledgement_payload;

    for (std::size_t i = 1; i < command.size(); ++i) {
        const auto& channel = command[i];

        subscriptions.insert(channel);
        _channel_subscribers[channel].insert(client_id);

        acknowledgement_payload += resp::serialize_response(resp::ResponseArray{
            .values = {
                resp::BulkString{.value = "subscribe"},
                resp::BulkString{.value = channel},
                resp::Integer{.value = static_cast<std::int64_t>(subscriptions.size())},
            },
        });
    }

    auto frame = std::make_shared<const PubSubFrame>(PubSubFrame{
        .payload = std::move(acknowledgement_payload),
    });
    auto enqueue_result = session->enqueue(std::move(frame));

    if (!enqueue_result) {
        return resp::SimpleError{
            .value = "ERR failed to queue Pub/Sub acknowledgement: " + enqueue_result.error(),
        };
    }

    return resp::NoResponse{};
}

resp::Response CommandProcessor::unsubscribe(std::uint64_t client_id,
                                             const resp::Command& command) {
    const auto client = _clients.find(client_id);

    if (client == _clients.end()) {
        return resp::SimpleError{
            .value = "ERR Pub/Sub session is not registered",
        };
    }

    auto session = client->second.pubsub_session.lock();

    if (!session) {
        _clients.erase(client);
        return resp::SimpleError{
            .value = "ERR Pub/Sub session is no longer available",
        };
    }

    auto remove_subscription = [this, client_id](const std::string& channel) {
        auto subscriptions = _client_subscriptions.find(client_id);

        if (subscriptions == _client_subscriptions.end()) {
            return std::size_t{0};
        }

        if (subscriptions->second.erase(channel) != 0) {
            auto channel_entry = _channel_subscribers.find(channel);

            if (channel_entry != _channel_subscribers.end()) {
                channel_entry->second.erase(client_id);

                if (channel_entry->second.empty()) {
                    _channel_subscribers.erase(channel_entry);
                }
            }
        }

        const std::size_t remaining = subscriptions->second.size();

        if (subscriptions->second.empty()) {
            _client_subscriptions.erase(subscriptions);
        }

        return remaining;
    };

    std::string acknowledgement_payload;

    if (command.size() == 1) {
        auto subscriptions = _client_subscriptions.find(client_id);

        if (subscriptions == _client_subscriptions.end() || subscriptions->second.empty()) {
            acknowledgement_payload = resp::serialize_response(resp::ResponseArray{
                .values = {
                    resp::BulkString{.value = "unsubscribe"},
                    resp::NullBulkString{},
                    resp::Integer{.value = 0},
                },
            });
        } else {
            std::vector<std::string> channels{subscriptions->second.begin(),
                                              subscriptions->second.end()};

            for (const auto& channel : channels) {
                const std::size_t remaining = remove_subscription(channel);

                acknowledgement_payload += resp::serialize_response(resp::ResponseArray{
                    .values = {
                        resp::BulkString{.value = "unsubscribe"},
                        resp::BulkString{.value = channel},
                        resp::Integer{.value = static_cast<std::int64_t>(remaining)},
                    },
                });
            }
        }
    } else {
        for (std::size_t i = 1; i < command.size(); ++i) {
            const auto& channel = command[i];
            const std::size_t remaining = remove_subscription(channel);

            acknowledgement_payload += resp::serialize_response(resp::ResponseArray{
                .values = {
                    resp::BulkString{.value = "unsubscribe"},
                    resp::BulkString{.value = channel},
                    resp::Integer{.value = static_cast<std::int64_t>(remaining)},
                },
            });
        }
    }

    auto frame = std::make_shared<const PubSubFrame>(PubSubFrame{
        .payload = std::move(acknowledgement_payload),
    });
    auto enqueue_result = session->enqueue(std::move(frame));

    if (!enqueue_result) {
        return resp::SimpleError{
            .value = "ERR failed to queue Pub/Sub acknowledgement: " + enqueue_result.error(),
        };
    }

    return resp::NoResponse{};
}

resp::Response CommandProcessor::publish(const resp::Command& command) {
    if (command.size() != 3) {
        return resp::SimpleError{
            .value = "ERR wrong number of arguments for 'publish' command",
        };
    }

    auto subscribers = _channel_subscribers.find(command[1]);

    if (subscribers == _channel_subscribers.end()) {
        return resp::Integer{.value = 0};
    }

    auto frame = std::make_shared<const PubSubFrame>(PubSubFrame{
        .payload = resp::serialize_response(resp::Array{
            .values = {"message", command[1], command[2]},
        }),
    });

    std::int64_t recipients = 0;
    auto subscriber = subscribers->second.begin();

    while (subscriber != subscribers->second.end()) {
        const std::uint64_t client_id = *subscriber;
        const auto client = _clients.find(client_id);
        std::shared_ptr<PubSubSession> session;

        if (client != _clients.end()) {
            session = client->second.pubsub_session.lock();
        }

        if (!session) {
            if (auto client_channels = _client_subscriptions.find(client_id);
                client_channels != _client_subscriptions.end()) {
                client_channels->second.erase(command[1]);

                if (client_channels->second.empty()) {
                    _client_subscriptions.erase(client_channels);
                }
            }

            _clients.erase(client_id);
            subscriber = subscribers->second.erase(subscriber);
            continue;
        }

        if (session->enqueue(frame)) {
            ++recipients;
        }

        ++subscriber;
    }

    if (subscribers->second.empty()) {
        _channel_subscribers.erase(subscribers);
    }

    return resp::Integer{.value = recipients};
}

std::expected<void, std::string>
CommandProcessor::enqueue_pubsub_response(std::uint64_t client_id, resp::Response response) {
    const auto client = _clients.find(client_id);

    if (client == _clients.end()) {
        return std::unexpected("Pub/Sub session is not registered");
    }

    auto session = client->second.pubsub_session.lock();

    if (!session) {
        _clients.erase(client);
        return std::unexpected("Pub/Sub session is no longer available");
    }

    auto frame = std::make_shared<const PubSubFrame>(PubSubFrame{
        .payload = resp::serialize_response(std::move(response)),
    });

    return session->enqueue(std::move(frame));
}

resp::Response CommandProcessor::process_acl_command(std::uint64_t client_id,
                                                     const resp::Command& command) {
    if (command.size() < 2) {
        return resp::SimpleError{
            .value = "ERR wrong number of arguments for 'acl' command",
        };
    }

    if (command[1] == "WHOAMI") {
        return acl_whoami(client_id, command);
    }

    if (command[1] == "GETUSER") {
        return acl_getuser(command);
    }

    if (command[1] == "SETUSER") {
        return acl_setuser(command);
    }

    return resp::SimpleError{
        .value = "ERR unknown ACL subcommand",
    };
}

resp::Response CommandProcessor::acl_whoami(std::uint64_t client_id,
                                            const resp::Command& command) const {
    if (command.size() != 2) {
        return resp::SimpleError{
            .value = "ERR wrong number of arguments for 'acl|whoami' command",
        };
    }

    const auto client = _clients.find(client_id);

    if (client == _clients.end() || !client->second.authenticated_username) {
        return resp::SimpleError{
            .value = "NOAUTH Authentication required.",
        };
    }

    return resp::BulkString{
        .value = *client->second.authenticated_username,
    };
}

resp::Response CommandProcessor::acl_getuser(const resp::Command& command) const {
    if (command.size() != 3) {
        return resp::SimpleError{
            .value = "ERR wrong number of arguments for 'acl|getuser' command",
        };
    }

    const auto user_entry = _acl_users.find(command[2]);

    if (user_entry == _acl_users.end()) {
        return resp::NullArray{};
    }

    const auto& user = user_entry->second;

    std::vector<std::string> flags;
    flags.push_back(user.enabled ? "on" : "off");

    if (user.all_keys) {
        flags.emplace_back("allkeys");
    }

    if (user.all_channels) {
        flags.emplace_back("allchannels");
    }

    if (user.all_commands) {
        flags.emplace_back("allcommands");
    }

    if (user.no_password) {
        flags.emplace_back("nopass");
    }

    std::vector<std::string> passwords{user.password_hashes.begin(), user.password_hashes.end()};
    std::ranges::sort(passwords);

    std::vector<std::string> allowed_commands{user.allowed_commands.begin(),
                                              user.allowed_commands.end()};
    std::vector<std::string> denied_commands{user.denied_commands.begin(),
                                             user.denied_commands.end()};
    std::ranges::sort(allowed_commands);
    std::ranges::sort(denied_commands);

    auto append_rule = [](std::string& rules, std::string_view prefix,
                          const std::string& value) {
        if (!rules.empty()) {
            rules += ' ';
        }

        rules += prefix;
        rules += value;
    };

    std::string command_rules = user.all_commands ? "+@all" : "-@all";

    for (const auto& denied : denied_commands) {
        append_rule(command_rules, "-", denied);
    }

    for (const auto& allowed : allowed_commands) {
        append_rule(command_rules, "+", allowed);
    }

    std::vector<std::string> key_patterns = user.key_patterns;
    std::ranges::sort(key_patterns);

    std::string key_rules;

    if (user.all_keys) {
        key_rules = "~*";
    } else {
        for (const auto& pattern : key_patterns) {
            append_rule(key_rules, "~", pattern);
        }
    }

    std::vector<std::string> channel_patterns = user.channel_patterns;
    std::ranges::sort(channel_patterns);

    std::string channel_rules;

    if (user.all_channels) {
        channel_rules = "&*";
    } else {
        for (const auto& pattern : channel_patterns) {
            append_rule(channel_rules, "&", pattern);
        }
    }

    return resp::ResponseArray{
        .values = {
            resp::BulkString{.value = "flags"},
            resp::Array{.values = std::move(flags)},
            resp::BulkString{.value = "passwords"},
            resp::Array{.values = std::move(passwords)},
            resp::BulkString{.value = "commands"},
            resp::BulkString{.value = std::move(command_rules)},
            resp::BulkString{.value = "keys"},
            resp::BulkString{.value = std::move(key_rules)},
            resp::BulkString{.value = "channels"},
            resp::BulkString{.value = std::move(channel_rules)},
            resp::BulkString{.value = "selectors"},
            resp::EmptyArray{},
        },
    };
}

resp::Response CommandProcessor::acl_setuser(const resp::Command& command) {
    if (command.size() < 3) {
        return resp::SimpleError{
            .value = "ERR wrong number of arguments for 'acl|setuser' command",
        };
    }

    const auto existing_user = _acl_users.find(command[2]);
    acl::User updated_user;

    if (existing_user != _acl_users.end()) {
        updated_user = existing_user->second;
    }

    auto normalize_command_name = [](std::string_view name) {
        std::string normalized{name};

        for (char& character : normalized) {
            if (character >= 'a' && character <= 'z') {
                character = static_cast<char>(character - 'a' + 'A');
            }
        }

        return normalized;
    };

    auto add_pattern = [](std::vector<std::string>& patterns, std::string_view pattern) {
        if (std::ranges::find(patterns, pattern) == patterns.end()) {
            patterns.emplace_back(pattern);
        }
    };

    auto normalize_password_hash = [](std::string_view hash) -> std::optional<std::string> {
        if (hash.size() != 64) {
            return std::nullopt;
        }

        std::string normalized;
        normalized.reserve(hash.size());

        for (const char character : hash) {
            if ((character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f')) {
                normalized.push_back(character);
            } else if (character >= 'A' && character <= 'F') {
                normalized.push_back(static_cast<char>(character - 'A' + 'a'));
            } else {
                return std::nullopt;
            }
        }

        return normalized;
    };

    for (std::size_t i = 3; i < command.size(); ++i) {
        const auto& rule = command[i];

        if (rule == "on") {
            updated_user.enabled = true;
        } else if (rule == "off") {
            updated_user.enabled = false;
        } else if (rule == "nopass") {
            updated_user.no_password = true;
            updated_user.password_hashes.clear();
        } else if (rule == "resetpass") {
            updated_user.no_password = false;
            updated_user.password_hashes.clear();
        } else if (rule == "allcommands") {
            updated_user.all_commands = true;
            updated_user.allowed_commands.clear();
            updated_user.denied_commands.clear();
        } else if (rule == "+@all") {
            updated_user.all_commands = true;
            updated_user.allowed_commands.clear();
            updated_user.denied_commands.clear();
        } else if (rule == "nocommands") {
            updated_user.all_commands = false;
            updated_user.allowed_commands.clear();
            updated_user.denied_commands.clear();
        } else if (rule == "-@all") {
            updated_user.all_commands = false;
            updated_user.allowed_commands.clear();
            updated_user.denied_commands.clear();
        } else if (rule == "allkeys") {
            updated_user.all_keys = true;
            updated_user.key_patterns.clear();
        } else if (rule == "resetkeys") {
            updated_user.all_keys = false;
            updated_user.key_patterns.clear();
        } else if (rule == "allchannels") {
            updated_user.all_channels = true;
            updated_user.channel_patterns.clear();
        } else if (rule == "resetchannels") {
            updated_user.all_channels = false;
            updated_user.channel_patterns.clear();
        } else if (rule == "reset") {
            updated_user = acl::User{};
        } else if (rule.size() > 1 && rule.front() == '>') {
            auto password_hash =
                crypto_utils::sha256(std::string_view{rule}.substr(1));

            if (!password_hash) {
                return resp::SimpleError{
                    .value = "ERR failed to hash ACL password: " + password_hash.error(),
                };
            }

            updated_user.no_password = false;
            updated_user.password_hashes.insert(std::move(*password_hash));
        } else if (rule.size() > 1 && rule.front() == '<') {
            auto password_hash =
                crypto_utils::sha256(std::string_view{rule}.substr(1));

            if (!password_hash) {
                return resp::SimpleError{
                    .value = "ERR failed to hash ACL password: " + password_hash.error(),
                };
            }

            updated_user.password_hashes.erase(*password_hash);
        } else if (rule.size() > 1 && rule.front() == '#') {
            auto password_hash = normalize_password_hash(std::string_view{rule}.substr(1));

            if (!password_hash) {
                return resp::SimpleError{
                    .value = "ERR ACL password hash must contain exactly 64 hexadecimal characters",
                };
            }

            updated_user.no_password = false;
            updated_user.password_hashes.insert(std::move(*password_hash));
        } else if (rule.size() > 1 && rule.front() == '!') {
            auto password_hash = normalize_password_hash(std::string_view{rule}.substr(1));

            if (!password_hash) {
                return resp::SimpleError{
                    .value = "ERR ACL password hash must contain exactly 64 hexadecimal characters",
                };
            }

            updated_user.password_hashes.erase(*password_hash);
        } else if (rule.size() > 1 && rule.front() == '+' && rule[1] != '@') {
            std::string command_name = normalize_command_name(
                std::string_view{rule}.substr(1));

            updated_user.denied_commands.erase(command_name);

            if (updated_user.all_commands) {
                updated_user.allowed_commands.erase(command_name);
            } else {
                updated_user.allowed_commands.insert(std::move(command_name));
            }
        } else if (rule.size() > 1 && rule.front() == '-' && rule[1] != '@') {
            std::string command_name = normalize_command_name(
                std::string_view{rule}.substr(1));

            updated_user.allowed_commands.erase(command_name);

            if (updated_user.all_commands) {
                updated_user.denied_commands.insert(std::move(command_name));
            } else {
                updated_user.denied_commands.erase(command_name);
            }
        } else if (rule.size() > 1 && rule.front() == '~') {
            const std::string_view pattern{rule.data() + 1, rule.size() - 1};

            if (pattern == "*") {
                updated_user.all_keys = true;
                updated_user.key_patterns.clear();
            } else if (!updated_user.all_keys) {
                add_pattern(updated_user.key_patterns, pattern);
            }
        } else if (rule.size() > 1 && rule.front() == '&') {
            const std::string_view pattern{rule.data() + 1, rule.size() - 1};

            if (pattern == "*") {
                updated_user.all_channels = true;
                updated_user.channel_patterns.clear();
            } else if (!updated_user.all_channels) {
                add_pattern(updated_user.channel_patterns, pattern);
            }
        } else {
            return resp::SimpleError{
                .value = "ERR unsupported ACL rule: " + rule,
            };
        }
    }

    _acl_users.insert_or_assign(command[2], std::move(updated_user));
    return resp::SimpleString{.value = "OK"};
}

resp::Response CommandProcessor::authenticate(std::uint64_t client_id,
                                              const resp::Command& command) {
    if (command.size() != 2 && command.size() != 3) {
        return resp::SimpleError{
            .value = "ERR wrong number of arguments for 'auth' command",
        };
    }

    const std::string_view username = command.size() == 2 ? "default" : command[1];
    const std::string_view password = command.size() == 2 ? command[1] : command[2];

    const auto client = _clients.find(client_id);

    if (client == _clients.end()) {
        return resp::SimpleError{
            .value = "ERR client connection is not registered",
        };
    }

    const auto user = _acl_users.find(std::string{username});

    if (user == _acl_users.end() || !user->second.enabled) {
        return resp::SimpleError{
            .value = "WRONGPASS invalid username-password pair or user is disabled.",
        };
    }

    bool credentials_match = user->second.no_password;

    if (!credentials_match) {
        auto password_hash = crypto_utils::sha256(password);

        if (!password_hash) {
            return resp::SimpleError{
                .value = "ERR failed to hash authentication password: " +
                         password_hash.error(),
            };
        }

        credentials_match = user->second.password_hashes.contains(*password_hash);
    }

    if (!credentials_match) {
        return resp::SimpleError{
            .value = "WRONGPASS invalid username-password pair or user is disabled.",
        };
    }

    client->second.authenticated_username = std::string{username};
    return resp::SimpleString{.value = "OK"};
}

std::size_t CommandProcessor::count_acknowledged_replicas(std::uint64_t target_offset) {
    std::size_t count = 0;

    std::erase_if(_replica_sessions, [this, target_offset, &count](const auto& weak_session) {
        auto session = weak_session.lock();

        if (!session) {
            return true;
        }

        if (target_offset == 0) {
            ++count;
            return false;
        }

        const auto acknowledged = _replica_acknowledged_offsets.find(session->client_id());

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

    if (target_offset == 0 || acknowledged >= request->required_replicas) {
        task.response.set_value(resp::Integer{
            .value = static_cast<std::int64_t>(acknowledged),
        });
        return;
    }

    append_replication_frame(resp::serialize_command({"REPLCONF", "GETACK", "*"}));

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
            auto persistence = persist_blocking_pop(pending->task, pending->key);

            if (!persistence) {
                pending->task.response.set_value(resp::SimpleError{
                    .value = "MISCONF AOF persistence error: " + persistence.error(),
                });
                pending = _pending_list_pops.erase(pending);
                continue;
            }

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
        const std::size_t acknowledged = count_acknowledged_replicas(pending->target_offset);

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

        const std::size_t acknowledged = count_acknowledged_replicas(pending->target_offset);

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
        } else if (auto* installation_task = std::get_if<InstallReplicationStateTask>(&*task)) {
            process_replication_state_installation(std::move(*installation_task));
        } else if (auto* offset_task = std::get_if<GetReplicationOffsetTask>(&*task)) {
            process_replication_offset_request(std::move(*offset_task));
        } else if (auto* acknowledgement_task =
                       std::get_if<AcknowledgeReplicaTask>(&*task)) {
            process_replica_acknowledgement(std::move(*acknowledgement_task));
        } else if (auto* rdb_task = std::get_if<LoadRdbTask>(&*task)) {
            process_rdb_load(std::move(*rdb_task));
        } else if (auto* aof_task = std::get_if<InitializeAofTask>(&*task)) {
            process_aof_initialization(std::move(*aof_task));
        } else if (auto* registration_task = std::get_if<RegisterClientTask>(&*task)) {
            process_client_registration(std::move(*registration_task));
        } else {
            process_client_unregistration(std::move(std::get<UnregisterClientTask>(*task)));
        }

        retry_pending_list_pops();
        retry_pending_stream_reads();
        retry_pending_waits();
        expire_pending_list_pops();
        expire_pending_stream_reads();
        expire_pending_waits();
    }
}
