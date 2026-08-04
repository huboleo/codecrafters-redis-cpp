#include "replication.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

ReplicaSession::ReplicaSession(std::uint64_t client_id, std::uint64_t starting_offset)
    : _client_id(client_id), _starting_offset(starting_offset) {}

std::uint64_t ReplicaSession::client_id() const { return _client_id; }

std::uint64_t ReplicaSession::starting_offset() const { return _starting_offset; }

void ReplicaSession::enqueue(std::shared_ptr<const ReplicationFrame> frame) {
    {
        std::lock_guard lock(_mutex);

        if (_closed) {
            return;
        }

        _pending_frames.push_back(std::move(frame));
    }

    _frame_available.notify_one();
}

std::optional<std::shared_ptr<const ReplicationFrame>>
ReplicaSession::wait_for_frame(std::stop_token stop_token) {
    std::unique_lock lock(_mutex);

    _frame_available.wait(lock, stop_token,
                          [this] { return _closed || !_pending_frames.empty(); });

    if (stop_token.stop_requested() || _closed) {
        return std::nullopt;
    }

    auto frame = std::move(_pending_frames.front());
    _pending_frames.pop_front();

    return frame;
}

void ReplicaSession::close() {
    {
        std::lock_guard lock(_mutex);
        _closed = true;
    }

    _frame_available.notify_all();
}
