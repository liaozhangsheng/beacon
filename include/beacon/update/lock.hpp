#pragma once

#include <beacon/core/model.hpp>

#include <filesystem>

namespace beacon::update {

enum class LockMode { Shared, Exclusive };

// Beacon and `--check` hold a shared lock; operations that modify the
// installation hold an exclusive lock. The lock file is permanent: unlinking it
// would allow two different lock inodes.
class InstallLock {
public:
    // Fails with ErrorCode::Validation when another process holds a conflicting
    // lock, and with ErrorCode::Io when the lock file cannot be opened.
    static ylt::expected<InstallLock, Error> acquire(const std::filesystem::path& root, LockMode mode);

    InstallLock(InstallLock&& other) noexcept;
    InstallLock& operator=(InstallLock&& other) noexcept;
    InstallLock(const InstallLock&) = delete;
    InstallLock& operator=(const InstallLock&) = delete;
    ~InstallLock();

private:
#ifdef _WIN32
    using Handle = void*;
    static constexpr Handle invalid_handle = nullptr;
#else
    using Handle = int;
    static constexpr Handle invalid_handle = -1;
#endif
    explicit InstallLock(Handle handle) : handle_(handle) {}
    void release() noexcept;

    Handle handle_ = invalid_handle;
};

}  // namespace beacon::update
