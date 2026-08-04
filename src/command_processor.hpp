#pragma once

#include "database.hpp"
#include "replication.hpp"
#include "resp.hpp"
#include "server_config.hpp"
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

class CommandProcessor {
  public:
    explicit CommandProcessor(const std::optional<ReplicaConfig>& replica_of);
    ~CommandProcessor();

    CommandProcessor(const CommandProcessor&) = delete;
    CommandProcessor& operator=(const CommandProcessor&) = delete;

    [[nodiscard]] std::future<resp::Response> submit(std::uint64_t client_id,
                                                     resp::Command command);

    [[nodiscard]] std::future<resp::Response>
    apply_replication(resp::Command command, std::size_t bytes_consumed);

    [[nodiscard]] std::future<void>
    install_replication_state(std::string replication_id, std::uint64_t offset);

    [[nodiscard]] std::future<std::uint64_t> replication_offset();

    [[nodiscard]] std::future<void>
    acknowledge_replica(std::uint64_t client_id, std::uint64_t offset);

    [[nodiscard]] std::future<ReplicaRegistration> register_replica(std::uint64_t client_id);

  private:
    enum class ReplicationRole { MASTER, REPLICA };
    enum class CommandSource { CLIENT, MASTER };

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

    using ProcessorTask = std::variant<Task, RegisterReplicaTask,
                                       InstallReplicationStateTask,
                                       GetReplicationOffsetTask,
                                       AcknowledgeReplicaTask>;

    struct ReplicationState {
        ReplicationRole role;
        std::string replication_id;
        std::uint64_t offset;
    };

    Database _database;

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

    std::mutex _task_mutex;
    std::condition_variable _task_available;

    std::jthread _worker;

    [[nodiscard]] std::future<resp::Response>
    enqueue(std::uint64_t client_id, resp::Command command, CommandSource source,
            std::size_t replication_bytes);

    void run(std::stop_token stop_token);

    void process_task(Task task);

    void process_replica_registration(RegisterReplicaTask task);

    void process_replication_state_installation(InstallReplicationStateTask task);

    void process_replication_offset_request(GetReplicationOffsetTask task);

    void process_replica_acknowledgement(AcknowledgeReplicaTask task);

    resp::Response process_command(std::uint64_t client_id, resp::Command& command,
                                   CommandSource source);

    void append_replication_frame(std::string payload);

    void append_client_replication_frame(std::uint64_t client_id, std::string payload);

    void append_blocking_pop_frame(const Task& task, const std::string& key);

    void process_blpop(Task task);

    void process_xread(Task task);

    void process_wait(Task task);

    void retry_pending_list_pops();

    void retry_pending_stream_reads();

    void retry_pending_waits();

    void expire_pending_list_pops();

    void expire_pending_stream_reads();

    void expire_pending_waits();

    [[nodiscard]] std::size_t
    count_acknowledged_replicas(std::uint64_t target_offset);

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_pending_deadline() const;

    resp::Response execute_transaction(std::uint64_t client_id, CommandSource source);

    [[nodiscard]] std::string_view get_replication_role() const;
};
