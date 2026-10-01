#include "temporary_directory.hpp"

#include <beacon/update/lock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

using beacon::update::InstallLock;
using beacon::update::LockMode;

TEST_CASE("Beacon and the updater cannot use an installation simultaneously", "[update]") {
    beacon::test::TemporaryDirectory directory;
    const auto& root = directory.path;
    {
        auto application = InstallLock::acquire(root, LockMode::Shared);
        REQUIRE(application);
        REQUIRE(InstallLock::acquire(root, LockMode::Shared));
        const auto updater = InstallLock::acquire(root, LockMode::Exclusive);
        REQUIRE_FALSE(updater);
        REQUIRE(updater.error().code == beacon::ErrorCode::Validation);
    }
    {
        auto updater = InstallLock::acquire(root, LockMode::Exclusive);
        REQUIRE(updater);
        REQUIRE_FALSE(InstallLock::acquire(root, LockMode::Shared));
        REQUIRE_FALSE(InstallLock::acquire(root, LockMode::Exclusive));

        // Moving a lock transfers ownership instead of releasing it.
        auto moved = std::move(*updater);
        REQUIRE_FALSE(InstallLock::acquire(root, LockMode::Shared));
    }
    REQUIRE(InstallLock::acquire(root, LockMode::Exclusive));
}

TEST_CASE("An unusable lock location is reported as an I/O error", "[update]") {
    beacon::test::TemporaryDirectory directory;
    const auto missing = InstallLock::acquire(directory.path / "missing", LockMode::Shared);
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().code == beacon::ErrorCode::Io);
}
