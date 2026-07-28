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
    std::size_t result_size;
    {
        std::lock_guard lock(_mutex);

        auto [entry, inserted] = _lists.try_emplace(std::move(key));

        auto& list = entry->second;

        if (mode == ListAddMode::APPEND) {
            list.insert(list.end(), std::make_move_iterator(values.begin()),
                        std::make_move_iterator(values.end()));
        } else {
            list.insert(list.begin(), std::make_move_iterator(values.rbegin()),
                        std::make_move_iterator(values.rend()));
        }

        result_size = list.size();
    }

    _list_changed.notify_all();

    return result_size;
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

std::size_t Database::get_list_length(const std::string& key) {
    std::lock_guard lock(_mutex);
    auto entry = _lists.find(key);

    if (entry == _lists.end()) {
        return 0;
    }

    return entry->second.size();
}

std::optional<std::string> Database::pop_list_element(const std::string& key) {
    std::lock_guard lock(_mutex);

    auto entry = _lists.find(key);

    if (entry == _lists.end()) {
        return std::nullopt;
    }

    auto& list = entry->second;

    if (list.empty()) {
        return std::nullopt;
    }

    std::string deleted = std::move(list.front());
    list.pop_front();

    return deleted;
}

std::optional<std::vector<std::string>> Database::pop_list_elements(const std::string& key,
                                                                    int number_of_items) {
    std::lock_guard lock(_mutex);

    auto entry = _lists.find(key);

    if (entry == _lists.end()) {
        return std::nullopt;
    }

    auto& list = entry->second;

    if (list.empty()) {
        return std::nullopt;
    }

    std::vector<std::string> result{};

    if (number_of_items > list.size()) {
        number_of_items = list.size();
    }

    for (std::size_t i{0}; i < number_of_items; ++i) {
        result.push_back(std::move(list.front()));
        list.pop_front();
    }

    return result;
}

std::optional<std::string>
Database::pop_list_element_blocking(const std::string& key, std::optional<Milliseconds> timeout) {
    std::unique_lock lock(_mutex);

    auto has_element = [this, &key] {
        auto entry = _lists.find(key);

        return entry != _lists.end() and !entry->second.empty();
    };

    if (timeout) {
        const bool became_ready = _list_changed.wait_for(lock, *timeout, has_element);

        if (!became_ready) {
            return std::nullopt;
        }
    } else {
        _list_changed.wait(lock, has_element);
    }

    auto entry = _lists.find(key);
    auto& list = entry->second;

    std::string value = std::move(list.front());
    list.pop_front();

    return value;
}
