#include "pubsub.hpp"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

PubSubSession::PubSubSession(int notification_fd) : _notification_fd(notification_fd) {}

std::expected<std::shared_ptr<PubSubSession>, std::string> PubSubSession::create() {
    const int notification_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    if (notification_fd < 0) {
        return std::unexpected("Failed to create Pub/Sub notification descriptor: " +
                               std::string{std::strerror(errno)});
    }

    return std::shared_ptr<PubSubSession>{new PubSubSession(notification_fd)};
}

PubSubSession::~PubSubSession() {
    if (_notification_fd >= 0) {
        close(_notification_fd);
    }
}

int PubSubSession::notification_fd() const { return _notification_fd; }

std::expected<void, std::string>
PubSubSession::enqueue(std::shared_ptr<const PubSubFrame> frame) {
    std::lock_guard lock(_mutex);
    const bool notification_needed = _pending_frames.empty();

    _pending_frames.push_back(std::move(frame));

    if (!notification_needed) {
        return {};
    }

    constexpr std::uint64_t notification = 1;

    while (true) {
        const ssize_t result = write(_notification_fd, &notification, sizeof(notification));

        if (result == static_cast<ssize_t>(sizeof(notification)) ||
            (result < 0 && errno == EAGAIN)) {
            return {};
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        _pending_frames.pop_back();

        return std::unexpected("Failed to notify Pub/Sub connection: " +
                               std::string{std::strerror(errno)});
    }
}

std::optional<std::shared_ptr<const PubSubFrame>> PubSubSession::pop_frame() {
    std::lock_guard lock(_mutex);

    if (_pending_frames.empty()) {
        return std::nullopt;
    }

    auto frame = std::move(_pending_frames.front());
    _pending_frames.pop_front();
    return frame;
}

std::expected<void, std::string> PubSubSession::clear_notification() {
    std::uint64_t notifications{};

    while (true) {
        const ssize_t result = read(_notification_fd, &notifications, sizeof(notifications));

        if (result == static_cast<ssize_t>(sizeof(notifications)) ||
            (result < 0 && errno == EAGAIN)) {
            return {};
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        return std::unexpected("Failed to clear Pub/Sub notification: " +
                               std::string{std::strerror(errno)});
    }
}
