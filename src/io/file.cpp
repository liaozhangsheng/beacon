#include <beacon/io/file.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <utility>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace beacon {

FileStamp file_stamp(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return {};
    const auto size = std::filesystem::file_size(path, ec);
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec)
        return {};
    return {.mtime = file_time_milliseconds(time), .size = size, .exists = true};
}

ylt::expected<std::string, Error> directory_identity(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto handle =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info{};
        const bool ok = GetFileInformationByHandle(handle, &info) != 0;
        CloseHandle(handle);
        if (ok && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return std::to_string(info.dwVolumeSerialNumber) + "/" + std::to_string(info.nFileIndexHigh) + "/" +
                   std::to_string(info.nFileIndexLow) + "/" + std::to_string(info.ftCreationTime.dwHighDateTime) + "/" +
                   std::to_string(info.ftCreationTime.dwLowDateTime);
        }
    }
#else
    struct stat info = {};
    if (::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
        auto identity = std::to_string(info.st_dev) + "/" + std::to_string(info.st_ino);
    #if defined(__APPLE__)
        identity +=
            "/" + std::to_string(info.st_birthtimespec.tv_sec) + "/" + std::to_string(info.st_birthtimespec.tv_nsec);
    #endif
        return identity;
    }
#endif
    return ylt::unexpected<Error>{{ErrorCode::Io, "cannot identify world directory", path_to_utf8(path)}};
}

ylt::expected<std::string, Error> read_bounded_file(const std::filesystem::path& path, const std::string_view kind,
                                                    const std::size_t max_bytes) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return ylt::unexpected<Error>{file_error(ErrorCode::Io, "cannot open " + std::string(kind), path)};
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return ylt::unexpected<Error>{file_error(ErrorCode::Io, "cannot inspect " + std::string(kind), path)};
    if (size > max_bytes)
        return ylt::unexpected<Error>{
            file_error(ErrorCode::SecurityLimit, std::string(kind) + " exceeds byte limit", path)};
    std::ifstream input(path, std::ios::binary);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size())))
        return ylt::unexpected<Error>{file_error(ErrorCode::Io, "cannot read " + std::string(kind), path)};
    return data;
}

ylt::expected<void, Error> sync_path(const std::filesystem::path& path, const bool directory, const bool barrier) {
    const auto failed = [&] {
        return ylt::unexpected<Error>{
            Error{.code = ErrorCode::Io, .message = "cannot flush path", .context = path_to_utf8(path)}};
    };
#if defined(_WIN32)
    // NTFS journals metadata and renames use MOVEFILE_WRITE_THROUGH.
    (void)barrier;
    if (directory) {
        return {};
    }
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool flushed = handle != INVALID_HANDLE_VALUE && FlushFileBuffers(handle) != FALSE;
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    if (!flushed) {
        return failed();
    }
    return {};
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
    if (descriptor < 0) {
        return failed();
    }
    int result = -1;
    #if defined(__APPLE__)
    if (barrier) {
        result = ::fcntl(descriptor, F_FULLFSYNC);
    }
    #else
    (void)barrier;
    #endif
    if (result != 0) {
        result = ::fsync(descriptor);
    }
    // Some filesystems cannot sync directories; retrying would not help.
    const bool ignored = result != 0 && directory && (errno == EINVAL || errno == ENOTSUP);
    ::close(descriptor);
    if (result != 0 && !ignored) {
        return failed();
    }
    return {};
#endif
}

ylt::expected<void, Error> sync_tree(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator entries(root, error);
    for (; !error && entries != std::filesystem::recursive_directory_iterator{}; entries.increment(error)) {
        const auto status = entries->symlink_status(error);
        if (error) {
            break;
        }
        const bool directory = std::filesystem::is_directory(status);
        if (directory || std::filesystem::is_regular_file(status)) {
            auto synced = sync_path(entries->path(), directory, false);
            if (!synced) {
                return synced;
            }
        }
    }
    if (error) {
        return ylt::unexpected<Error>{
            Error{.code = ErrorCode::Io, .message = "cannot list files to flush", .context = path_to_utf8(root)}};
    }
    return sync_path(root, true, true);
}

ylt::expected<void, Error> write_file_atomically(const std::filesystem::path& path, const std::string_view text) {
    const auto failed = [&](std::string message) {
        return ylt::unexpected<Error>{
            Error{.code = ErrorCode::Io, .message = std::move(message), .context = path_to_utf8(path)}};
    };
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return failed("cannot create directory");
        }
    }
    // The process ID separates instances; exclusive creation also handles stale
    // files left behind after a crash or process ID reuse.
    static std::atomic<std::uint64_t> sequence{0};
#if defined(_WIN32)
    const auto process = GetCurrentProcessId();
#else
    const auto process = ::getpid();
#endif
    std::filesystem::path temporary;
    std::FILE* output = nullptr;
    do {
        temporary = path;
        temporary +=
            ".tmp." + std::to_string(process) + "." + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
#if defined(_WIN32)
        output = ::_wfopen(temporary.c_str(), L"wbx");
#else
        output = std::fopen(temporary.c_str(), "wbx");
#endif
        if (output == nullptr && errno != EEXIST) {
            return failed("cannot create temporary file");
        }
    } while (output == nullptr);
    const auto written = std::fwrite(text.data(), 1, text.size(), output);
    const auto closed = std::fclose(output);
    if (written != text.size() || closed != 0) {
        std::filesystem::remove(temporary, error);
        return failed("cannot write file");
    }
    if (auto synced = sync_path(temporary, false); !synced) {
        std::filesystem::remove(temporary, error);
        return synced;
    }
#if defined(_WIN32)
    if (retry_while_file_busy([&] {
            return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
                   FALSE;
        })) {
        return {};
    }
    std::filesystem::remove(temporary, error);
    return failed("cannot replace file");
#else
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return failed("cannot replace file");
    }
    return sync_path(path.has_parent_path() ? path.parent_path() : std::filesystem::path("."), true);
#endif
}

}  // namespace beacon
