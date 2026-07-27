#include "database.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

void Database::set(std::string key, std::string value, std::optional<Milliseconds> expiry) {
    std::optional<Clock::time_point> expires_at;

    if (expiry) {
        expires_at = Clock::now() + *expiry;
    }

    std::lock_guard lock(_mutex);

    _entries.insert_or_assign(std::move(key),
                              Entry{.value = std::move(value), .expires_at = expires_at});
}

std::optional<std::string> Database::get(const std::string& key) {
    std::lock_guard lock(_mutex);

    auto entry = _entries.find(key);

    if (entry == _entries.end()) {
        return std::nullopt;
    }

    if (entry->second.expires_at and Clock::now() >= *entry->second.expires_at) {
        _entries.erase(entry);
        return std::nullopt;
    }

    return entry->second.value;
}
