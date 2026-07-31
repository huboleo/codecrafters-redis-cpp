#pragma once
#include "rstream_types.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

class Database {
  public:
    enum class ListAddMode {
        APPEND,
        PREPEND,
    };

    enum class Error {
        // Generic errors
        KEY_NOT_FOUND,
        WRONG_TYPE,

        // Stream specific error
        INVALID_STREAM_ID_MODE,
        INVALID_STREAM_ID_VALUE,

        // Incr
        VALUE_NOT_INTEGER
    };

    enum class ValueType {
        NONE,
        STRING,
        LIST,
        STREAM,
    };

    void set(std::string key, std::string value, std::optional<std::chrono::milliseconds> expiry);

    [[nodiscard]] std::expected<std::string, Error> get(const std::string& key);

    [[nodiscard]] ValueType get_type(const std::string& key);

    [[nodiscard]] std::expected<std::size_t, Error>
    add_list_elements(std::string key, std::vector<std::string> values, ListAddMode mode);

    // stop is inclusive
    [[nodiscard]] std::expected<std::vector<std::string>, Error>
    list_elements(const std::string& key, std::int64_t start, std::int64_t stop);

    [[nodiscard]] std::expected<std::size_t, Error> get_list_length(const std::string& key);

    [[nodiscard]] std::expected<std::string, Error> pop_list_element(const std::string& key);

    [[nodiscard]] std::expected<std::vector<std::string>, Error>
    pop_list_elements(const std::string& key, int number_of_items);

    [[nodiscard]] std::expected<rstream::StreamId, Error>
    add_stream(const std::string& key, rstream::IdRequest id,
               std::vector<std::pair<std::string, std::string>> values);

    [[nodiscard]] std::expected<std::vector<rstream::StreamEntry>, Error>
    stream_range(const std::string& key, rstream::StreamId start, rstream::StreamId end);

    // Resolves LATEST positions on the first call so the same requests can be retried later.
    [[nodiscard]] std::expected<std::vector<rstream::ReadResult>, Error>
    read_streams(std::vector<rstream::ReadRequest>& requests);

    [[nodiscard]] std::expected<std::int64_t, Error> increment_key(const std::string& key);

  private:
    struct Entry {
        std::variant<std::string, std::deque<std::string>, std::vector<rstream::StreamEntry>> value;
        std::optional<std::chrono::steady_clock::time_point> expires_at;
    };

    Entry* find_active_entry(const std::string& key);

    std::unordered_map<std::string, Entry> _entries;
};
