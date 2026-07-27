#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Database {
  public:
    enum class ListAddMode {
        APPEND,
        PREPEND,
    };

    using Milliseconds = std::chrono::milliseconds;

    void set(std::string key, std::string value, std::optional<Milliseconds> expiry);

    [[nodiscard]] std::optional<std::string> get(const std::string& key);

    [[nodiscard]] std::size_t add_list_elements(std::string key, std::vector<std::string> values,
                                                ListAddMode mode);

    // stop is inclusive
    [[nodiscard]] std::optional<std::vector<std::string>>
    list_elements(const std::string& key, std::int64_t start, std::int64_t stop);

    [[nodiscard]] std::size_t get_list_length(const std::string& key);

  private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        std::string value;
        std::optional<Clock::time_point> expires_at;
    };

    std::unordered_map<std::string, Entry> _entries;
    std::unordered_map<std::string, std::vector<std::string>> _lists;
    std::mutex _mutex;
};
