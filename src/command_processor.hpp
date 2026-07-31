#pragma once

#include "database.hpp"
#include "resp.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class CommandProcessor {
  public:
    CommandProcessor();
    ~CommandProcessor();

    CommandProcessor(const CommandProcessor&) = delete;
    CommandProcessor& operator=(const CommandProcessor&) = delete;

    [[nodiscard]] std::future<resp::Response> submit(std::uint64_t client_id,
                                                     resp::Command command);

  private:
    struct Task {
        std::uint64_t client_id;
        resp::Command command;
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

    Database _database;

    std::deque<Task> _tasks;
    std::deque<PendingListPop> _pending_list_pops;
    std::deque<PendingStreamRead> _pending_stream_reads;
    std::unordered_map<std::uint64_t, std::vector<resp::Command>> _transactions;

    std::mutex _task_mutex;
    std::condition_variable _task_available;

    std::jthread _worker;

    void run(std::stop_token stop_token);

    void process_task(Task task);

    resp::Response process_command(std::uint64_t client_id, resp::Command command);

    void process_blpop(Task task);

    void process_xread(Task task);

    void retry_pending_list_pops();

    void retry_pending_stream_reads();

    void expire_pending_list_pops();

    void expire_pending_stream_reads();

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_pending_deadline() const;

    resp::Response execute_transaction(std::uint64_t client_id);
};
