#include "database.hpp"
#include "rstream_types.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
std::optional<std::int64_t> parse_value_to_increment(std::string_view text) {
    std::int64_t result{};

    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);

    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }

    return result;
}
} // namespace

Database::Entry* Database::find_active_entry(const std::string& key) {
    auto entry = _entries.find(key);

    if (entry == _entries.end()) {
        return nullptr;
    }

    if (entry->second.expires_at && std::chrono::steady_clock::now() >= *entry->second.expires_at) {
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

    _entries.insert_or_assign(std::move(key),
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

    if (std::holds_alternative<std::vector<rstream::StreamEntry>>(entry->value)) {
        return ValueType::STREAM;
    }

    return ValueType::NONE;
}

std::expected<std::size_t, Database::Error>
Database::add_list_elements(std::string key, std::vector<std::string> values, ListAddMode mode) {
    std::size_t result_size;

    {
        std::lock_guard lock(_mutex);

        Entry* entry = find_active_entry(key);

        if (!entry) {
            auto inserted_entry =
                _entries
                    .emplace(std::move(key),
                             Entry{.value = std::deque<std::string>{}, .expires_at = std::nullopt})
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
Database::list_elements(const std::string& key, std::int64_t start, std::int64_t stop) {
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

    return std::vector<std::string>(list->begin() + start, list->begin() + stop + 1);
}

std::expected<std::size_t, Database::Error> Database::get_list_length(const std::string& key) {
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

std::expected<std::string, Database::Error> Database::pop_list_element(const std::string& key) {
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

    const auto count = std::min(static_cast<std::size_t>(number_of_items), list->size());

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
Database::pop_list_element_blocking(const std::string& key,
                                    std::optional<std::chrono::milliseconds> timeout) {
    std::unique_lock lock(_mutex);

    if (Entry* entry = find_active_entry(key);
        entry && !std::holds_alternative<std::deque<std::string>>(entry->value)) {
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

std::expected<rstream::StreamId, Database::Error>
Database::add_stream(const std::string& key, rstream::IdRequest id,
                     std::vector<std::pair<std::string, std::string>> values) {
    rstream::StreamId new_stream_id{};

    {
        std::lock_guard lock(_mutex);

        Entry* entry = find_active_entry(key);

        if (!entry) {
            auto inserted_entry = _entries.emplace(key, std::vector<rstream::StreamEntry>{}).first;

            entry = &inserted_entry->second;
        }

        auto* stream = std::get_if<std::vector<rstream::StreamEntry>>(&entry->value);

        if (!stream) {
            return std::unexpected(Error::WRONG_TYPE);
        }
        const bool has_entries = !stream->empty();

        const rstream::StreamId last_id = has_entries ? stream->back().id : rstream::StreamId{};

        switch (id.mode) {
        case rstream::IdRequest::Mode::EXPLICIT:
            new_stream_id = rstream::StreamId{
                .milliseconds = id.milliseconds,
                .sequence = id.sequence,
            };
            break;

        case rstream::IdRequest::Mode::AUTOMATIC_SEQUENCE: {
            std::uint64_t sequence = id.milliseconds == 0 ? 1 : 0;

            if (has_entries && id.milliseconds == last_id.milliseconds) {
                sequence = last_id.sequence + 1;
            }

            new_stream_id = rstream::StreamId{
                .milliseconds = id.milliseconds,
                .sequence = sequence,
            };

            break;
        }
        case rstream::IdRequest::Mode::AUTOMATIC: {
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();

            const auto current_time = static_cast<std::uint64_t>(milliseconds);

            if (has_entries && current_time <= last_id.milliseconds) {
                new_stream_id = rstream::StreamId{
                    .milliseconds = last_id.milliseconds,
                    .sequence = last_id.sequence + 1,
                };
            } else {
                new_stream_id = rstream::StreamId{
                    .milliseconds = current_time,
                    .sequence = 0,
                };
            }

            break;
        }

        default:
            return std::unexpected(Error::INVALID_STREAM_ID_MODE);
        }

        if (has_entries && new_stream_id <= last_id) {
            return std::unexpected(Error::INVALID_STREAM_ID_VALUE);
        }

        stream->push_back(rstream::StreamEntry{
            .id = new_stream_id,
            .values = std::move(values),
        });
    }

    _stream_entry_added.notify_all();

    return new_stream_id;
}

std::expected<std::vector<rstream::StreamEntry>, Database::Error>
Database::stream_range(const std::string& key, rstream::StreamId start, rstream::StreamId end) {

    std::lock_guard lock(_mutex);

    Entry* entry = find_active_entry(key);

    if (!entry) {
        return std::unexpected(Database::Error::KEY_NOT_FOUND);
    }

    auto* stream = std::get_if<std::vector<rstream::StreamEntry>>(&entry->value);

    if (!stream) {
        return std::unexpected(Database::Error::WRONG_TYPE);
    }

    auto first =
        std::ranges::lower_bound(*stream, start, std::ranges::less{}, &rstream::StreamEntry::id);

    auto last =
        std::ranges::upper_bound(*stream, end, std::ranges::less{}, &rstream::StreamEntry::id);

    return std::vector<rstream::StreamEntry>(first, last);
}

std::expected<std::vector<rstream::ReadResult>, Database::Error>
Database::read_streams(std::vector<rstream::ReadRequest> requests, rstream::ReadOptions options) {
    std::unique_lock lock(_mutex);

    for (auto& request : requests) {
        Entry* entry = find_active_entry(request.key);

        if (entry == nullptr) {
            if (request.start_mode == rstream::ReadStartMode::LATEST) {
                request.after_id = rstream::StreamId{};
                request.start_mode = rstream::ReadStartMode::AFTER_ID;
            }
            continue;
        }

        auto* stream = std::get_if<std::vector<rstream::StreamEntry>>(&entry->value);

        if (stream == nullptr) {
            return std::unexpected(Error::WRONG_TYPE);
        }

        if (request.start_mode == rstream::ReadStartMode::LATEST) {
            request.after_id = stream->empty() ? rstream::StreamId{} : stream->back().id;

            request.start_mode = rstream::ReadStartMode::AFTER_ID;
        }
    }

    auto collect_entries = [this,
                            &requests]() -> std::expected<std::vector<rstream::ReadResult>, Error> {
        std::vector<rstream::ReadResult> results;
        results.reserve(requests.size());

        for (const auto& request : requests) {
            Entry* entry = find_active_entry(request.key);

            if (entry == nullptr) {
                continue;
            }

            auto* stream = std::get_if<std::vector<rstream::StreamEntry>>(&entry->value);

            if (stream == nullptr) {
                return std::unexpected(Error::WRONG_TYPE);
            }

            const auto first_entry =
                std::ranges::upper_bound(*stream, request.after_id, {}, &rstream::StreamEntry::id);

            if (first_entry == stream->end()) {
                continue;
            }

            results.push_back(
                rstream::ReadResult{.key = request.key, .entries = {first_entry, stream->end()}});
        }

        return results;
    };

    auto available_entries = collect_entries();

    if (!available_entries) {
        return std::unexpected(available_entries.error());
    }

    if (!available_entries->empty() || options.mode == rstream::ReadMode::IMMEDIATE) {
        return available_entries;
    }

    auto entries_are_available = [this, &requests] {
        for (const auto& request : requests) {
            Entry* entry = find_active_entry(request.key);

            if (entry == nullptr) {
                continue;
            }

            auto* stream = std::get_if<std::vector<rstream::StreamEntry>>(&entry->value);

            if (stream == nullptr) {
                continue;
            }

            const auto first_entry =
                std::ranges::upper_bound(*stream, request.after_id, {}, &rstream::StreamEntry::id);

            if (first_entry != stream->end()) {
                return true;
            }
        }

        return false;
    };

    if (options.mode == rstream::ReadMode::BLOCK_INDEFINITELY) {
        _stream_entry_added.wait(lock, entries_are_available);
    } else if (options.mode == rstream::ReadMode::BLOCK_WITH_TIMEOUT) {
        const bool entry_was_added =
            _stream_entry_added.wait_for(lock, options.timeout, entries_are_available);

        if (!entry_was_added) {
            return std::unexpected(Error::TIMEOUT);
        }
    }

    return collect_entries();
}

std::expected<std::int64_t, Database::Error> Database::increment_key(const std::string& key) {
    std::lock_guard lock(_mutex);

    std::int64_t result;

    Entry* entry = find_active_entry(key);

    if (entry != nullptr) {
        auto* text = std::get_if<std::string>(&entry->value);

        if (text == nullptr) {
            return std::unexpected(Database::Error::WRONG_TYPE);
        }

        auto parsed = parse_value_to_increment(*text);

        if (!parsed) {
            return std::unexpected(Database::Error::VALUE_NOT_INTEGER);
        }

        result = ++parsed.value();

        entry->value = std::to_string(result);
    } else {
        result = 1;
        _entries.emplace(key, std::to_string(result));
    }

    return result;
}
