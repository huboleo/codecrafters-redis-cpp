#pragma once
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class Database {
  public:
    using Milliseconds = std::chrono::milliseconds;

    void set(std::string key, std::string value, std::optional<Milliseconds> expiry);

    [[nodiscard]] std::optional<std::string> get(const std::string& key);

  private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        std::string value;
        std::optional<Clock::time_point> expires_at;
    };

    std::unordered_map<std::string, Entry> _entries;
    std::mutex _mutex;
};
