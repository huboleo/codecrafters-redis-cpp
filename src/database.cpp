#include "database.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

Database::Entry* Database::find_active_entry(const std::string& key) {
    auto entry = _entries.find(key);

    if (entry == _entries.end()) {
        return nullptr;
    }

    if (entry->second.expires_at &&
        std::chrono::steady_clock::now() >= *entry->second.expires_at) {
        _entries.erase(entry);
        return nullptr;
    }

    return &entry->second;
}

void Database::set(std::string key, std::string value,
                   std::optional<std::chrono::milliseconds> expiry) {
    std::optional<std::chrono::steady_clock::time_point> expires_at;

    if (expiry) {
        expires_at = std::chrono::steady_clock::now() + *expiry;
    }

    std::lock_guard lock(_mutex);

    _entries.insert_or_assign(
        std::move(key),
        Entry{.value = std::move(value), .expires_at = expires_at});
}

std::expected<std::string, Database::Error> Database::get(const std::string& key) {
    std::lock_guard lock(_mutex);

    Entry* entry = find_active_entry(key);

    if (!entry) {
        return std::unexpected(Error::KEY_NOT_FOUND);
    }

    auto* value = std::get_if<std::string>(&entry->value);

    if (!value) {
        return std::unexpected(Error::WRONG_TYPE);
    }

    return *value;
}

Database::ValueType Database::get_type(const std::string& key) {
    std::lock_guard lock(_mutex);

    Entry* entry = find_active_entry(key);

    if (!entry) {
        return ValueType::NONE;
    }

    if (std::holds_alternative<std::string>(entry->value)) {
        return ValueType::STRING;
    }

    if (std::holds_alternative<std::deque<std::string>>(entry->value)) {
        return ValueType::LIST;
    }

    return ValueType::NONE;
}

std::expected<std::size_t, Database::Error>
Database::add_list_elements(std::string key, std::vector<std::string> values,
                            ListAddMode mode) {
    std::size_t result_size;

    {
        std::lock_guard lock(_mutex);

        Entry* entry = find_active_entry(key);

        if (!entry) {
            auto inserted_entry =
                _entries
                    .emplace(
                        std::move(key),
                        Entry{.value = std::deque<std::string>{},
                              .expires_at = std::nullopt})
                    .first;

            entry = &inserted_entry->second;
        }

        auto* list = std::get_if<std::deque<std::string>>(&entry->value);

        if (!list) {
            return std::unexpected(Error::WRONG_TYPE);
        }

        if (mode == ListAddMode::APPEND) {
            list->insert(list->end(), std::make_move_iterator(values.begin()),
                         std::make_move_iterator(values.end()));
        } else {
            list->insert(list->begin(), std::make_move_iterator(values.rbegin()),
                         std::make_move_iterator(values.rend()));
        }

        result_size = list->size();
    }

    _list_changed.notify_all();

    return result_size;
}

std::expected<std::vector<std::string>, Database::Error>
Database::list_elements(const std::string& key, std::int64_t start,
                        std::int64_t stop) {
    std::lock_guard lock(_mutex);

    Entry* entry = find_active_entry(key);

    if (!entry) {
        return std::vector<std::string>{};
    }

    auto* list = std::get_if<std::deque<std::string>>(&entry->value);

    if (!list) {
        return std::unexpected(Error::WRONG_TYPE);
    }

    const auto size = static_cast<std::int64_t>(list->size());

    if (size == 0) {
        return std::vector<std::string>{};
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

    if (stop < 0 || start >= size || start > stop) {
        return std::vector<std::string>{};
    }

    return std::vector<std::string>(list->begin() + start,
                                    list->begin() + stop + 1);
}

std::expected<std::size_t, Database::Error>
Database::get_list_length(const std::string& key) {
    std::lock_guard lock(_mutex);

    Entry* entry = find_active_entry(key);

    if (!entry) {
        return 0;
    }

    auto* list = std::get_if<std::deque<std::string>>(&entry->value);

    if (!list) {
        return std::unexpected(Error::WRONG_TYPE);
    }

    return list->size();
}

std::expected<std::string, Database::Error>
Database::pop_list_element(const std::string& key) {
    std::lock_guard lock(_mutex);

    Entry* entry = find_active_entry(key);

    if (!entry) {
        return std::unexpected(Error::KEY_NOT_FOUND);
    }

    auto* list = std::get_if<std::deque<std::string>>(&entry->value);

    if (!list) {
        return std::unexpected(Error::WRONG_TYPE);
    }

    if (list->empty()) {
        return std::unexpected(Error::KEY_NOT_FOUND);
    }

    std::string result = std::move(list->front());
    list->pop_front();

    if (list->empty()) {
        _entries.erase(key);
    }

    return result;
}

std::expected<std::vector<std::string>, Database::Error>
Database::pop_list_elements(const std::string& key, int number_of_items) {
    std::lock_guard lock(_mutex);

    Entry* entry = find_active_entry(key);

    if (!entry) {
        return std::unexpected(Error::KEY_NOT_FOUND);
    }

    auto* list = std::get_if<std::deque<std::string>>(&entry->value);

    if (!list) {
        return std::unexpected(Error::WRONG_TYPE);
    }

    if (list->empty()) {
        return std::unexpected(Error::KEY_NOT_FOUND);
    }

    if (number_of_items <= 0) {
        return std::vector<std::string>{};
    }

    const auto count =
        std::min(static_cast<std::size_t>(number_of_items), list->size());

    std::vector<std::string> result;
    result.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(std::move(list->front()));
        list->pop_front();
    }

    if (list->empty()) {
        _entries.erase(key);
    }

    return result;
}

std::expected<std::string, Database::Error>
Database::pop_list_element_blocking(
    const std::string& key,
    std::optional<std::chrono::milliseconds> timeout) {
    std::unique_lock lock(_mutex);

    if (Entry* entry = find_active_entry(key);
        entry &&
        !std::holds_alternative<std::deque<std::string>>(entry->value)) {
        return std::unexpected(Error::WRONG_TYPE);
    }

    auto has_element = [this, &key] {
        Entry* entry = find_active_entry(key);

        if (!entry) {
            return false;
        }

        auto* list = std::get_if<std::deque<std::string>>(&entry->value);
        return list && !list->empty();
    };

    if (timeout && *timeout > std::chrono::milliseconds::zero()) {
        if (!_list_changed.wait_for(lock, *timeout, has_element)) {
            return std::unexpected(Error::TIMEOUT);
        }
    } else {
        _list_changed.wait(lock, has_element);
    }

    Entry* entry = find_active_entry(key);
    auto* list = std::get_if<std::deque<std::string>>(&entry->value);

    std::string result = std::move(list->front());
    list->pop_front();

    if (list->empty()) {
        _entries.erase(key);
    }

    return result;
}
