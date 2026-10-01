#pragma once

#include <beacon/core/model.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace beacon::update {

// A release component names an immutable archive published by the update
// service and is identified by the archive's SHA-256. The engine never
// downloads; callers fetch changed archives with download_file() and pass them
// to Engine::apply().
//
// Components split a version directory by structure: `assets` is its assets/
// subtree and `runtime` is everything else.
struct Component {
    std::string url;
    std::string sha256;
    std::uint64_t size = 0;
};

struct Release {
    std::string version;
    std::string platform;
    Component runtime;
    Component assets;
};

struct UpdatePlan {
    bool runtime = false;
    bool assets = false;
    bool version_changed = false;

    // A newer version is an update even when both components are unchanged;
    // the new version directory then reuses both installed components.
    [[nodiscard]] bool available() const {
        return runtime || assets || version_changed;
    }
};

// Parses a signed release manifest after its detached signature has been
// verified by the caller.
ylt::expected<Release, Error> read_release(std::string_view json);

// Receives the prepared version directory before it is selected.
using HealthCheck = std::function<bool(const std::filesystem::path& program_directory)>;

// Installs complete versions side by side under <root>/versions and selects
// one by atomically replacing <root>/.beacon-current. The selected version is
// never modified, so an interrupted update needs no recovery: leftovers are
// discarded by the next apply().
//
// apply() and rollback() require the exclusive install lock; plan() and
// current_version() only read the installation.
class Engine {
public:
    explicit Engine(std::filesystem::path root);

    ylt::expected<std::string, Error> current_version() const;
    ylt::expected<UpdatePlan, Error> plan(const Release& release) const;

    // Archives are required exactly for the components the plan marks as
    // changed; unchanged components are copied from the current version.
    ylt::expected<void, Error> apply(const Release& release,
                                     std::optional<std::filesystem::path> runtime_archive = std::nullopt,
                                     std::optional<std::filesystem::path> assets_archive = std::nullopt,
                                     const HealthCheck& health_check = {});

    // Selects the newest installed version older than the current one and
    // returns it.
    ylt::expected<std::string, Error> rollback();

private:
    std::filesystem::path root_;
};

}  // namespace beacon::update
