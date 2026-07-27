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
#include <vector>

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

std::size_t Database::add_list_elements(std::string key, std::vector<std::string> values,
                                        ListAddMode mode) {
    std::lock_guard lock(_mutex);

    auto [entry, inserted] = _lists.try_emplace(std::move(key));

    auto& list = entry->second;

    list.reserve(list.size() + values.size());

    if (mode == ListAddMode::APPEND) {
        list.insert(list.end(), std::make_move_iterator(values.begin()),
                    std::make_move_iterator(values.end()));
    } else {
        list.insert(list.begin(), std::make_move_iterator(values.rbegin()),
                    std::make_move_iterator(values.rend()));
    }

    return list.size();
}

std::optional<std::vector<std::string>>
Database::list_elements(const std::string& key, std::int64_t start, std::int64_t stop) {

    std::lock_guard lock(_mutex);

    auto entry = _lists.find(key);

    if (entry == _lists.end()) {
        return std::nullopt;
    }

    const auto& list = entry->second;
    const auto size = static_cast<std::int64_t>(list.size());

    if (size == 0) {
        return std::nullopt;
    }

    if (start < 0) {
        start += size;
    }

    if (stop < 0) {
        stop += size;
    }

    if (start < 0) {
        start = 0;
    }

    if (stop >= size) {
        stop = size - 1;
    }

    if (stop < 0 or start >= size or start > stop) {
        return std::nullopt;
    }

    return std::vector<std::string>(list.begin() + start, list.begin() + stop + 1);
}
