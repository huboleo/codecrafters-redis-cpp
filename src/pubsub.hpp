#pragma once

#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct PubSubFrame {
    std::string payload;
};

class PubSubSession {
  public:
    [[nodiscard]] static std::expected<std::shared_ptr<PubSubSession>, std::string> create();

    ~PubSubSession();

    PubSubSession(const PubSubSession&) = delete;
    PubSubSession& operator=(const PubSubSession&) = delete;

    [[nodiscard]] int notification_fd() const;

    [[nodiscard]] std::expected<void, std::string>
    enqueue(std::shared_ptr<const PubSubFrame> frame);

    [[nodiscard]] std::optional<std::shared_ptr<const PubSubFrame>> pop_frame();

    [[nodiscard]] std::expected<void, std::string> clear_notification();

  private:
    explicit PubSubSession(int notification_fd);

    int _notification_fd;
    std::mutex _mutex;
    std::deque<std::shared_ptr<const PubSubFrame>> _pending_frames;
};
