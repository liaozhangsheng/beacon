#include <beacon/io/file.hpp>

#include <algorithm>
#include <cerrno>
#include <limits>
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

namespace {

#if defined(_WIN32)
constexpr std::int64_t windows_epoch_100ns = 116444736000000000LL;

std::int64_t windows_time_milliseconds(const LARGE_INTEGER time) {
    return (time.QuadPart - windows_epoch_100ns) / 10'000;
}
#endif

#if !defined(_WIN32)
std::int64_t unix_time_milliseconds(const std::int64_t seconds, const std::int64_t nanoseconds) {
    return (seconds * 1'000) + (nanoseconds / 1'000'000);
}
#endif

Error read_error(const ErrorCode code, std::string message, const std::filesystem::path& path) {
    return {.code = code, .message = std::move(message), .context = path_to_utf8(path.filename())};
}

}  // namespace

ylt::expected<FileStamp, Error> file_stamp(const std::filesystem::path& path) {
    FileStamp stamp;
#if defined(_WIN32)
    const auto handle =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return stamp;
    } else {
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_BASIC_INFO basic{};
        const bool ok = GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info) != 0 &&
                        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) != 0;
        CloseHandle(handle);
        if (ok && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            stamp.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            stamp.mtime = windows_time_milliseconds(basic.LastWriteTime);
            stamp.ctime = windows_time_milliseconds(basic.ChangeTime);
            stamp.identity = {info.dwVolumeSerialNumber,
                              (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
            stamp.exists = true;
            return stamp;
        }
    }
#else
    struct stat info = {};
    if (::stat(path.c_str(), &info) != 0) {
        if (errno == ENOENT)
            return stamp;
    } else if (S_ISREG(info.st_mode)) {
        stamp.size = static_cast<std::uintmax_t>(info.st_size);
        stamp.identity = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
    #if defined(__APPLE__)
        stamp.mtime = unix_time_milliseconds(info.st_mtimespec.tv_sec, info.st_mtimespec.tv_nsec);
        stamp.ctime = unix_time_milliseconds(info.st_ctimespec.tv_sec, info.st_ctimespec.tv_nsec);
    #else
        stamp.mtime = unix_time_milliseconds(info.st_mtim.tv_sec, info.st_mtim.tv_nsec);
        stamp.ctime = unix_time_milliseconds(info.st_ctim.tv_sec, info.st_ctim.tv_nsec);
    #endif
        stamp.exists = true;
        return stamp;
    }
#endif
    return ylt::unexpected<Error>{{ErrorCode::Io, "cannot inspect file", path_to_utf8(path)}};
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
#if defined(_WIN32)
    const auto handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return ylt::unexpected<Error>{read_error(ErrorCode::Io, "cannot open " + std::string(kind), path)};
    }
    const auto fail = [&](const ErrorCode code, std::string message) -> ylt::expected<std::string, Error> {
        CloseHandle(handle);
        return ylt::unexpected<Error>{read_error(code, std::move(message), path)};
    };

    BY_HANDLE_FILE_INFORMATION basic{};
    FILE_STANDARD_INFO standard{};
    if (GetFileInformationByHandle(handle, &basic) == 0 ||
        GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) == 0) {
        return fail(ErrorCode::Io, "cannot inspect " + std::string(kind));
    }
    if (basic.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT || standard.Directory) {
        return fail(ErrorCode::Io, "cannot read non-regular " + std::string(kind));
    }
    if (standard.EndOfFile.QuadPart < 0 || static_cast<unsigned long long>(standard.EndOfFile.QuadPart) > max_bytes ||
        static_cast<unsigned long long>(standard.EndOfFile.QuadPart) > std::numeric_limits<std::size_t>::max()) {
        return fail(ErrorCode::SecurityLimit, std::string(kind) + " exceeds byte limit");
    }

    const auto size = static_cast<std::size_t>(standard.EndOfFile.QuadPart);
    std::string data(size, '\0');
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto requested = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, MAXDWORD));
        DWORD count = 0;
        if (!ReadFile(handle, data.data() + offset, requested, &count, nullptr) || count == 0) {
            return fail(ErrorCode::Io, "cannot read " + std::string(kind));
        }
        offset += count;
    }

    char extra = 0;
    DWORD count = 0;
    if (!ReadFile(handle, &extra, 1, &count, nullptr)) {
        return fail(ErrorCode::Io, "cannot read " + std::string(kind));
    }
    if (count != 0) {
        return fail(ErrorCode::Io, std::string(kind) + " changed while reading");
    }
    CloseHandle(handle);
    return data;
#else
    int flags = O_RDONLY | O_NONBLOCK;
    #ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
    #endif
    #ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
    #endif
    const auto descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        return ylt::unexpected<Error>{read_error(ErrorCode::Io, "cannot open " + std::string(kind), path)};
    }
    const auto fail = [&](const ErrorCode code, std::string message) -> ylt::expected<std::string, Error> {
        ::close(descriptor);
        return ylt::unexpected<Error>{read_error(code, std::move(message), path)};
    };

    struct stat info = {};
    if (::fstat(descriptor, &info) != 0) {
        return fail(ErrorCode::Io, "cannot inspect " + std::string(kind));
    }
    if (!S_ISREG(info.st_mode)) {
        return fail(ErrorCode::Io, "cannot read non-regular " + std::string(kind));
    }
    if (info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) > max_bytes ||
        static_cast<std::uintmax_t>(info.st_size) > std::numeric_limits<std::size_t>::max()) {
        return fail(ErrorCode::SecurityLimit, std::string(kind) + " exceeds byte limit");
    }

    const auto size = static_cast<std::size_t>(info.st_size);
    std::string data(size, '\0');
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::read(descriptor, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return fail(ErrorCode::Io, "cannot read " + std::string(kind));
        offset += static_cast<std::size_t>(count);
    }

    char extra = 0;
    while (true) {
        const auto count = ::read(descriptor, &extra, 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return fail(ErrorCode::Io, "cannot read " + std::string(kind));
        if (count > 0)
            return fail(ErrorCode::Io, std::string(kind) + " changed while reading");
        break;
    }
    ::close(descriptor);
    return data;
#endif
}

}  // namespace beacon
