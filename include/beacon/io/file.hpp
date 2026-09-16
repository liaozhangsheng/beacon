#pragma once

#include <beacon/core/model.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

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
