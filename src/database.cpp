#include "database.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
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

std::size_t Database::append_list_elements(std::string key, std::vector<std::string> values) {
    std::lock_guard lock(_mutex);

    auto [entry, inserted] = _lists.try_emplace(std::move(key));

    auto& list = entry->second;

    list.reserve(list.size() + values.size());

    list.insert(list.end(), std::make_move_iterator(values.begin()),
                std::make_move_iterator(values.end()));

    return list.size();
}
