#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

struct ReplicationFrame {
    std::string payload;
    std::uint64_t ending_offset;
};

class ReplicaSession {
  public:
    ReplicaSession(std::uint64_t client_id, std::uint64_t starting_offset);

    [[nodiscard]] std::uint64_t client_id() const;
    [[nodiscard]] std::uint64_t starting_offset() const;

    void enqueue(std::shared_ptr<const ReplicationFrame> frame);

    [[nodiscard]] std::optional<std::shared_ptr<const ReplicationFrame>>
    wait_for_frame(std::stop_token stop_token);

    void close();

  private:
    std::uint64_t _client_id;
    std::uint64_t _starting_offset;

    std::mutex _mutex;
    std::condition_variable_any _frame_available;
    std::deque<std::shared_ptr<const ReplicationFrame>> _pending_frames;
    bool _closed = false;
};

struct ReplicaRegistration {
    std::string replication_id;
    std::uint64_t starting_offset;
    std::shared_ptr<ReplicaSession> session;
};
