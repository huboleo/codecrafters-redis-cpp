#pragma once

#include "resp.hpp"
#include "server_config.hpp"
#include <chrono>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace aof {

class AppendOnlyFile {
  public:
    ~AppendOnlyFile();

    AppendOnlyFile(const AppendOnlyFile&) = delete;
    AppendOnlyFile& operator=(const AppendOnlyFile&) = delete;

    AppendOnlyFile(AppendOnlyFile&& other) noexcept;
    AppendOnlyFile& operator=(AppendOnlyFile&& other) noexcept;

    [[nodiscard]] static std::expected<AppendOnlyFile, std::string>
    open(const std::filesystem::path& base_directory, const AOFConfig& config);

    [[nodiscard]] std::expected<std::vector<resp::Command>, std::string>
    read_commands() const;

    [[nodiscard]] std::expected<void, std::string> append(std::string_view payload);

  private:
    enum class SyncPolicy {
        ALWAYS,
        EVERY_SECOND,
        NEVER,
    };

    AppendOnlyFile(int file_descriptor, std::filesystem::path path, SyncPolicy sync_policy);

    [[nodiscard]] std::expected<void, std::string> sync();

    int _file_descriptor{-1};
    std::filesystem::path _path;
    SyncPolicy _sync_policy;
    std::chrono::steady_clock::time_point _last_sync;
};

} // namespace aof
