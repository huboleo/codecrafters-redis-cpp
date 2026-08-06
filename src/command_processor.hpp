#pragma once

#include "acl.hpp"
#include "aof.hpp"
#include "database.hpp"
#include "pubsub.hpp"
#include "rdb.hpp"
#include "replication.hpp"
#include "resp.hpp"
#include "server_config.hpp"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

class CommandProcessor {
  public:
    explicit CommandProcessor(const std::optional<ReplicaConfig>& replica_of,
                              const RDBConfig& rdb_config, const AOFConfig& aof_config,
                              const std::optional<std::string>& default_user_password_hash);
    ~CommandProcessor();

    CommandProcessor(const CommandProcessor&) = delete;
    CommandProcessor& operator=(const CommandProcessor&) = delete;

    [[nodiscard]] std::future<resp::Response> submit(std::uint64_t client_id,
                                                     resp::Command command);

    [[nodiscard]] std::future<resp::Response> apply_replication(resp::Command command,
                                                                std::size_t bytes_consumed);

    [[nodiscard]] std::future<void> install_replication_state(std::string replication_id,
                                                              std::uint64_t offset);

    [[nodiscard]] std::future<std::uint64_t> replication_offset();

    [[nodiscard]] std::future<void> acknowledge_replica(std::uint64_t client_id,
                                                        std::uint64_t offset);

    [[nodiscard]] std::future<ReplicaRegistration> register_replica(std::uint64_t client_id);

    [[nodiscard]] std::future<void> load_rdb_entries(std::vector<rdb::StringEntry> entries);

    [[nodiscard]] std::future<std::expected<void, std::string>>
    initialize_aof(aof::AppendOnlyFile file, std::vector<resp::Command> commands);

    [[nodiscard]] std::future<void>
    register_client(std::uint64_t client_id, std::shared_ptr<PubSubSession> pubsub_session);

    [[nodiscard]] std::future<void> unregister_client(std::uint64_t client_id);

  private:
    enum class ReplicationRole { MASTER, REPLICA };
    enum class CommandSource { CLIENT, MASTER, AOF };

    struct ClientState {
        bool authenticated;
        std::weak_ptr<PubSubSession> pubsub_session;
    };

    struct Task {
        std::uint64_t client_id;
        resp::Command command;
        CommandSource source;
        std::size_t replication_bytes;
        std::promise<resp::Response> response;
    };

    struct PendingListPop {
        Task task;
        std::string key;
        std::optional<std::chrono::steady_clock::time_point> deadline;
    };

    struct PendingStreamRead {
        Task task;
        std::vector<rstream::ReadRequest> requests;
        std::optional<std::chrono::steady_clock::time_point> deadline;
    };

    struct PendingWait {
        Task task;
        std::size_t required_replicas;
        std::uint64_t target_offset;
        std::optional<std::chrono::steady_clock::time_point> deadline;
    };

    struct TransactionState {
        std::vector<resp::Command> commands;
        bool has_error = false;
    };

    struct RegisterReplicaTask {
        std::uint64_t client_id;
        std::promise<ReplicaRegistration> registration;
    };

    struct InstallReplicationStateTask {
        std::string replication_id;
        std::uint64_t offset;
        std::promise<void> completion;
    };

    struct GetReplicationOffsetTask {
        std::promise<std::uint64_t> offset;
    };

    struct AcknowledgeReplicaTask {
        std::uint64_t client_id;
        std::uint64_t offset;
        std::promise<void> completion;
    };

    struct LoadRdbTask {
        std::vector<rdb::StringEntry> entries;
        std::promise<void> completion;
    };

    struct InitializeAofTask {
        aof::AppendOnlyFile file;
        std::vector<resp::Command> commands;
        std::promise<std::expected<void, std::string>> completion;
    };

    struct RegisterClientTask {
        std::uint64_t client_id;
        std::shared_ptr<PubSubSession> pubsub_session;
        std::promise<void> completion;
    };

    struct UnregisterClientTask {
        std::uint64_t client_id;
        std::promise<void> completion;
    };

    using ProcessorTask = std::variant<Task, RegisterReplicaTask, InstallReplicationStateTask,
                                       GetReplicationOffsetTask, AcknowledgeReplicaTask,
                                       LoadRdbTask, InitializeAofTask, RegisterClientTask,
                                       UnregisterClientTask>;

    struct ReplicationState {
        ReplicationRole role;
        std::string replication_id;
        std::uint64_t offset;
    };

    Database _database;

    acl::User _default_user;

    RDBConfig _rdb_config;

    AOFConfig _aof_config;

    std::optional<aof::AppendOnlyFile> _aof_file;

    ReplicationState _replication_state;

    // Main task queue
    std::deque<ProcessorTask> _tasks;

    // Queues for pending blocking operations
    std::deque<PendingListPop> _pending_list_pops;
    std::deque<PendingStreamRead> _pending_stream_reads;
    std::deque<PendingWait> _pending_waits;

    // Ordered stream of writes produced by this master
    std::deque<std::shared_ptr<const ReplicationFrame>> _replication_backlog;
    std::vector<std::weak_ptr<ReplicaSession>> _replica_sessions;
    std::unordered_map<std::uint64_t, std::uint64_t> _replica_acknowledged_offsets;
    std::unordered_map<std::uint64_t, std::uint64_t> _client_write_offsets;

    // Transactions associated with clients
    std::unordered_map<std::uint64_t, TransactionState> _transactions;

    // Watched keys associated with clients. Each client has its own version of the database entry,
    // from the time of WATCH execution
    std::unordered_map<std::uint64_t, std::unordered_map<std::string, std::uint64_t>> _watched_keys;

    // Per-connection state
    std::unordered_map<std::uint64_t, ClientState> _clients;

    // Pub/Sub subscriptions
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> _channel_subscribers;
    std::unordered_map<std::uint64_t, std::unordered_set<std::string>> _client_subscriptions;

    std::mutex _task_mutex;
    std::condition_variable _task_available;

    std::jthread _worker;

    [[nodiscard]] std::future<resp::Response> enqueue(std::uint64_t client_id,
                                                      resp::Command command, CommandSource source,
                                                      std::size_t replication_bytes);

    void run(std::stop_token stop_token);

    void process_task(Task task);

    void process_replica_registration(RegisterReplicaTask task);

    void process_replication_state_installation(InstallReplicationStateTask task);

    void process_replication_offset_request(GetReplicationOffsetTask task);

    void process_replica_acknowledgement(AcknowledgeReplicaTask task);

    void process_rdb_load(LoadRdbTask task);

    void process_aof_initialization(InitializeAofTask task);

    void process_client_registration(RegisterClientTask task);

    void process_client_unregistration(UnregisterClientTask task);

    void cancel_pending_commands(std::uint64_t client_id);

    resp::Response subscribe(std::uint64_t client_id, const resp::Command& command);

    resp::Response unsubscribe(std::uint64_t client_id, const resp::Command& command);

    resp::Response publish(const resp::Command& command);

    [[nodiscard]] std::expected<void, std::string>
    enqueue_pubsub_response(std::uint64_t client_id, resp::Response response);

    resp::Response process_acl_command(std::uint64_t client_id,
                                       const resp::Command& command);

    resp::Response acl_whoami(std::uint64_t client_id, const resp::Command& command) const;

    resp::Response acl_getuser(const resp::Command& command) const;

    resp::Response acl_setuser(const resp::Command& command);

    resp::Response authenticate(std::uint64_t client_id, const resp::Command& command);

    resp::Response process_command(std::uint64_t client_id, resp::Command& command,
                                   CommandSource source);

    void append_replication_frame(std::string payload);

    void append_client_replication_frame(std::uint64_t client_id, std::string payload);

    [[nodiscard]] std::expected<void, std::string>
    persist_blocking_pop(const Task& task, const std::string& key);

    void process_blpop(Task task);

    void process_xread(Task task);

    void process_wait(Task task);

    void retry_pending_list_pops();

    void retry_pending_stream_reads();

    void retry_pending_waits();

    void expire_pending_list_pops();

    void expire_pending_stream_reads();

    void expire_pending_waits();

    [[nodiscard]] std::size_t count_acknowledged_replicas(std::uint64_t target_offset);

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_pending_deadline() const;

    resp::Response execute_transaction(std::uint64_t client_id, CommandSource source);

    [[nodiscard]] std::string_view get_replication_role() const;
};
