#pragma once

#include <beacon/core/model.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

namespace beacon {

struct FileStamp {
    // Millisecond precision keeps filesystem comparisons consistent across platforms.
    // The file contents are still compared where a timestamp alone is insufficient.
    std::int64_t mtime = 0;
    std::int64_t ctime = 0;
    std::array<std::uint64_t, 2> identity{};
    std::uintmax_t size = 0;
    bool exists = false;

    bool operator==(const FileStamp&) const = default;
};

inline std::int64_t file_time_milliseconds(const std::filesystem::file_time_type time) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count());
}

// Missing files are represented by exists=false; other I/O errors remain errors.
ylt::expected<FileStamp, Error> file_stamp(const std::filesystem::path& path);
// Identifies the directory itself, not replaceable files inside it.
ylt::expected<std::string, Error> directory_identity(const std::filesystem::path& path);

ylt::expected<std::string, Error> read_bounded_file(const std::filesystem::path& path, std::string_view kind,
                                                    std::size_t max_bytes = max_json_bytes);

// Flushes a file or directory to stable storage. A barrier also flushes the
// drive's cache where fsync does not (F_FULLFSYNC on macOS); one barrier after
// a batch of plain flushes is enough.
ylt::expected<void, Error> sync_path(const std::filesystem::path& path, bool directory, bool barrier = true);
// Flushes every file and directory below root, then root itself as a barrier.
ylt::expected<void, Error> sync_tree(const std::filesystem::path& root);
// Readers see either the old or the new content, never a partial file, and the
// new content is on stable storage before it replaces the old one.
ylt::expected<void, Error> write_file_atomically(const std::filesystem::path& path, std::string_view text);

// Scanners and indexers on Windows may briefly hold a file that was just written or used,
// so a failing operation is retried there for up to two seconds. Elsewhere it runs once.
template <typename Operation> bool retry_while_file_busy(Operation operation) {
#if defined(_WIN32)
    for (int attempt = 0; attempt != 20; ++attempt) {
        if (operation())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
#else
    return operation();
#endif
}

// Drops the line ending from a one-line file or process output.
inline void trim_line_end(std::string& text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
}

// Filesystem paths and external text formats use different encodings on Windows.
inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline std::filesystem::path path_from_utf8(const std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

}  // namespace beacon
