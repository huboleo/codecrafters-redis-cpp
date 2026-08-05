#include "aof.hpp"
#include "resp.hpp"
#include "server_config.hpp"
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct ManifestEntry {
    std::string filename;
    std::uint64_t sequence;
};

[[nodiscard]] std::expected<std::uint64_t, std::string>
parse_sequence(std::string_view text) {
    std::uint64_t sequence{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), sequence);

    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected("Invalid sequence number in AOF manifest");
    }

    return sequence;
}

[[nodiscard]] bool is_safe_manifest_filename(std::string_view filename) {
    if (filename.empty()) {
        return false;
    }

    const std::filesystem::path path{filename};
    return !path.is_absolute() && !path.has_parent_path() && path != "." && path != "..";
}

[[nodiscard]] std::expected<std::string, std::string>
read_active_filename(const std::filesystem::path& manifest_path) {
    std::ifstream manifest{manifest_path};

    if (!manifest) {
        return std::unexpected("Cannot open AOF manifest '" + manifest_path.string() + "'");
    }

    std::optional<ManifestEntry> active_entry;
    std::string line;

    while (std::getline(manifest, line)) {
        std::istringstream fields{line};
        std::string file_label;
        std::string filename;
        std::string sequence_label;
        std::string sequence_text;
        std::string type_label;
        std::string type;
        std::string extra;

        if (!(fields >> file_label >> filename >> sequence_label >> sequence_text >> type_label >>
              type) ||
            fields >> extra || file_label != "file" || sequence_label != "seq" ||
            type_label != "type") {
            return std::unexpected("Invalid entry in AOF manifest '" +
                                   manifest_path.string() + "'");
        }

        if (type != "i") {
            continue;
        }

        if (!is_safe_manifest_filename(filename)) {
            return std::unexpected("Invalid incremental filename in AOF manifest");
        }

        auto sequence = parse_sequence(sequence_text);

        if (!sequence) {
            return std::unexpected(sequence.error());
        }

        if (!active_entry || *sequence > active_entry->sequence) {
            active_entry = ManifestEntry{
                .filename = std::move(filename),
                .sequence = *sequence,
            };
        }
    }

    if (manifest.bad()) {
        return std::unexpected("Failed while reading AOF manifest '" +
                               manifest_path.string() + "'");
    }

    if (!active_entry) {
        return std::unexpected("AOF manifest does not contain an incremental file");
    }

    return std::move(active_entry->filename);
}

[[nodiscard]] std::expected<std::string, std::string>
create_manifest(const std::filesystem::path& manifest_path, const AOFConfig& config) {
    std::string active_filename = config.append_filename + ".1.incr.aof";
    std::ofstream manifest{manifest_path, std::ios::out | std::ios::trunc};

    if (!manifest) {
        return std::unexpected("Cannot create AOF manifest '" + manifest_path.string() + "'");
    }

    manifest << "file " << active_filename << " seq 1 type i\n";
    manifest.flush();

    if (!manifest) {
        return std::unexpected("Cannot write AOF manifest '" + manifest_path.string() + "'");
    }

    return active_filename;
}

} // namespace

aof::AppendOnlyFile::AppendOnlyFile(int file_descriptor, std::filesystem::path path,
                                    SyncPolicy sync_policy)
    : _file_descriptor(file_descriptor), _path(std::move(path)), _sync_policy(sync_policy),
      _last_sync(std::chrono::steady_clock::now()) {}

aof::AppendOnlyFile::~AppendOnlyFile() {
    if (_file_descriptor >= 0) {
        close(_file_descriptor);
    }
}

aof::AppendOnlyFile::AppendOnlyFile(AppendOnlyFile&& other) noexcept
    : _file_descriptor(std::exchange(other._file_descriptor, -1)),
      _path(std::move(other._path)), _sync_policy(other._sync_policy),
      _last_sync(other._last_sync) {}

aof::AppendOnlyFile& aof::AppendOnlyFile::operator=(AppendOnlyFile&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (_file_descriptor >= 0) {
        close(_file_descriptor);
    }

    _file_descriptor = std::exchange(other._file_descriptor, -1);
    _path = std::move(other._path);
    _sync_policy = other._sync_policy;
    _last_sync = other._last_sync;
    return *this;
}

std::expected<aof::AppendOnlyFile, std::string>
aof::AppendOnlyFile::open(const std::filesystem::path& base_directory,
                          const AOFConfig& config) {
    const std::filesystem::path append_directory =
        base_directory / config.append_dirname;
    std::error_code error;
    std::filesystem::create_directories(append_directory, error);

    if (error) {
        return std::unexpected("Cannot create AOF directory '" + append_directory.string() +
                               "': " + error.message());
    }

    const std::filesystem::path manifest_path =
        append_directory / (config.append_filename + ".manifest");
    const bool manifest_exists = std::filesystem::exists(manifest_path, error);

    if (error) {
        return std::unexpected("Cannot inspect AOF manifest '" + manifest_path.string() +
                               "': " + error.message());
    }

    std::expected<std::string, std::string> active_filename =
        manifest_exists ? read_active_filename(manifest_path)
                        : create_manifest(manifest_path, config);

    if (!active_filename) {
        return std::unexpected(active_filename.error());
    }

    SyncPolicy sync_policy;

    if (config.append_fsync == "always") {
        sync_policy = SyncPolicy::ALWAYS;
    } else if (config.append_fsync == "everysec") {
        sync_policy = SyncPolicy::EVERY_SECOND;
    } else if (config.append_fsync == "no") {
        sync_policy = SyncPolicy::NEVER;
    } else {
        return std::unexpected("Unsupported appendfsync policy");
    }

    const std::filesystem::path active_path = append_directory / *active_filename;
    const int file_descriptor =
        ::open(active_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (file_descriptor < 0) {
        return std::unexpected("Cannot open AOF file '" + active_path.string() + "': " +
                               std::strerror(errno));
    }

    return AppendOnlyFile{file_descriptor, active_path, sync_policy};
}

std::expected<std::vector<resp::Command>, std::string>
aof::AppendOnlyFile::read_commands() const {
    std::ifstream file{_path, std::ios::binary};

    if (!file) {
        return std::unexpected("Cannot read AOF file '" + _path.string() + "'");
    }

    const std::string contents{std::istreambuf_iterator<char>{file},
                               std::istreambuf_iterator<char>{}};

    if (file.bad()) {
        return std::unexpected("Failed while reading AOF file '" + _path.string() + "'");
    }

    std::vector<resp::Command> commands;
    std::size_t position = 0;

    while (position < contents.size()) {
        resp::ParseOutcome outcome =
            resp::parse_command(std::string_view{contents}.substr(position));

        if (auto* parsed = std::get_if<resp::ParseResult>(&outcome)) {
            if (parsed->bytes_consumed == 0) {
                return std::unexpected("AOF parser made no progress at byte " +
                                       std::to_string(position));
            }

            commands.push_back(std::move(parsed->command));
            position += parsed->bytes_consumed;
            continue;
        }

        if (std::holds_alternative<resp::Incomplete>(outcome)) {
            return std::unexpected("Incomplete command in AOF file at byte " +
                                   std::to_string(position));
        }

        const auto& parse_error = std::get<resp::ParseError>(outcome);
        return std::unexpected("Invalid command in AOF file at byte " +
                               std::to_string(position) + ": " + parse_error.message);
    }

    return commands;
}

std::expected<void, std::string> aof::AppendOnlyFile::sync() {
    if (::fsync(_file_descriptor) != 0) {
        return std::unexpected("Cannot sync AOF file '" + _path.string() + "': " +
                               std::strerror(errno));
    }

    _last_sync = std::chrono::steady_clock::now();
    return {};
}

std::expected<void, std::string> aof::AppendOnlyFile::append(std::string_view payload) {
    std::size_t written = 0;

    while (written < payload.size()) {
        const ssize_t result =
            ::write(_file_descriptor, payload.data() + written, payload.size() - written);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return std::unexpected("Cannot append to AOF file '" + _path.string() + "': " +
                                   std::strerror(errno));
        }

        if (result == 0) {
            return std::unexpected("AOF write made no progress");
        }

        written += static_cast<std::size_t>(result);
    }

    if (_sync_policy == SyncPolicy::ALWAYS) {
        return sync();
    }

    const auto now = std::chrono::steady_clock::now();

    if (_sync_policy == SyncPolicy::EVERY_SECOND &&
        now - _last_sync >= std::chrono::seconds{1}) {
        return sync();
    }

    return {};
}
