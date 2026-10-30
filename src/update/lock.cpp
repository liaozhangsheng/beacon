#include <beacon/update/lock.hpp>
#include <beacon/io/file.hpp>

#include <utility>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/file.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace beacon::update {

namespace {

constexpr auto lock_name = ".beacon-running.lock";

Error unavailable(const std::filesystem::path& path) {
    return {.code = ErrorCode::Io,
            .message = "Cannot open installation lock; check installation directory permissions",
            .context = path_to_utf8(path)};
}

Error busy(const LockMode mode) {
    return {.code = ErrorCode::Validation,
            .message = mode == LockMode::Shared
                           ? "An update is in progress; wait for it to finish, then start Beacon again"
                           : "Installation is busy; close Beacon and other updater processes, then retry",
            .context = "install lock"};
}

}  // namespace

ylt::expected<InstallLock, Error> InstallLock::acquire(const std::filesystem::path& root, const LockMode mode) {
    const auto path = root / lock_name;
    const bool exclusive = mode == LockMode::Exclusive;
#ifdef _WIN32
    const auto handle =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return ylt::unexpected<Error>{unavailable(path)};
    InstallLock lock(handle);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
        return ylt::unexpected<Error>{unavailable(path)};
    OVERLAPPED overlap{};
    if (!LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1, 0, &overlap))
        return ylt::unexpected<Error>{busy(mode)};
    return lock;
#else
    const auto descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return ylt::unexpected<Error>{unavailable(path)};
    InstallLock lock(descriptor);
    struct stat information = {};
    if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode))
        return ylt::unexpected<Error>{unavailable(path)};
    if (flock(descriptor, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
        return ylt::unexpected<Error>{busy(mode)};
    return lock;
#endif
}

InstallLock::InstallLock(InstallLock&& other) noexcept : handle_(std::exchange(other.handle_, invalid_handle)) {}

InstallLock& InstallLock::operator=(InstallLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, invalid_handle);
    }
    return *this;
}

InstallLock::~InstallLock() {
    release();
}

void InstallLock::release() noexcept {
    if (handle_ == invalid_handle)
        return;
#ifdef _WIN32
    CloseHandle(handle_);
#else
    ::close(handle_);
#endif
    handle_ = invalid_handle;
}

}  // namespace beacon::update
