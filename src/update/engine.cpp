#include <beacon/update/engine.hpp>
#include <beacon/io/file.hpp>
#include <beacon/update/download.hpp>
#include <beacon/update/layout.hpp>
#include <beacon/update/version.hpp>

#include "../core/json.hpp"
#include "common.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace beacon::update {

namespace {

constexpr std::uint64_t max_package_bytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t max_component_bytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t max_metadata_bytes = 64U * 1024U;

enum class ComponentKind { Runtime, Assets };

// Stored as <version directory>/.beacon-version.json. A component is
// identified by the SHA-256 of the archive it was installed from.
struct VersionMetadata {
    std::string version;
    std::string platform;
    std::string runtime;
    std::string assets;
};

struct Installed {
    VersionMetadata metadata;
    std::filesystem::path directory;
};

// ---------------------------------------------------------------------------
// Value validation

// The package is covered by the signed manifest, so only paths that could
// leave the destination or that Windows would reinterpret are rejected here;
// the release script checks portability when packaging.
bool valid_relative_path(const std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find_first_of("\\:") != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = (std::min)(path.find('/', begin), path.size());
        const auto part = path.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        begin = end + 1;
    }
    return true;
}

// Components are split by structure: assets/ belongs to assets, everything
// else in a version directory except its metadata belongs to the runtime.
bool belongs_to(const std::string_view path, const ComponentKind kind, const bool directory) {
    if (kind == ComponentKind::Assets) {
        return path.starts_with("assets/") || (directory && path == "assets");
    }
    return path != "assets" && !path.starts_with("assets/") && path != layout::version_metadata;
}

bool valid_token(const std::string_view value, const std::size_t max_length) {
    if (value.empty() || value.size() > max_length) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return std::isalnum(byte) != 0 || ch == '-' || ch == '_' || ch == '.';
    });
}

// Shares the downloader's rules, so a release accepted by --check cannot fail
// on apply because of its URL.
bool valid_url(const std::string_view value) {
    return value.size() <= 2'048 && http::is_https_url(value);
}

// ---------------------------------------------------------------------------
// Filesystem primitives

ylt::expected<void, Error> ensure_directory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        return failure(ErrorCode::Io, "cannot create directory", path_to_utf8(path));
    }
    return {};
}

// Removes a file or directory tree; a link is removed without following it.
ylt::expected<void, Error> remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) {
        return failure(ErrorCode::Io, "cannot remove path", path_to_utf8(path));
    }
    return {};
}

ylt::expected<void, Error> rename_path(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (retry_while_file_busy([&] {
            std::error_code error;
            std::filesystem::rename(from, to, error);
            return !error;
        })) {
        return {};
    }
    return failure(ErrorCode::Io, "cannot rename path", path_to_utf8(to));
}

ylt::expected<void, Error> write_file(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) {
        return failure(ErrorCode::Io, "cannot write file", path_to_utf8(path));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Release manifest and version metadata

std::string json_string(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["commentStyle"] = "None";
    builder["indentation"] = "  ";
    return Json::writeString(builder, value) + "\n";
}

std::optional<std::string> string_member(const Json::Value& object, const std::string_view name) {
    const auto& value = object[std::string(name)];
    if (!value.isString()) {
        return std::nullopt;
    }
    return value.asString();
}

bool unsigned_member(const Json::Value& object, const std::string_view name, std::uint64_t& result) {
    const auto& value = object[std::string(name)];
    if (!(value.isUInt64() || value.isUInt() || value.isInt()) || (value.isInt() && value.asInt() < 0)) {
        return false;
    }
    result = value.asLargestUInt();
    return true;
}

ylt::expected<Component, Error> validate_component(const Component& component, const std::string_view context) {
    if (!valid_url(component.url) || !valid_sha256(component.sha256) || component.size == 0 ||
        component.size > max_package_bytes) {
        return failure(ErrorCode::Validation, "component values are invalid", std::string(context));
    }
    return Component{component.url, lower(component.sha256), component.size};
}

ylt::expected<Release, Error> validate_release(const Release& input) {
    if (!parse_version(input.version) || !valid_token(input.platform, 64)) {
        return failure(ErrorCode::Validation, "release version or platform is invalid", "release");
    }
    auto runtime = validate_component(input.runtime, "runtime");
    if (!runtime) {
        return ylt::unexpected<Error>{runtime.error()};
    }
    auto assets = validate_component(input.assets, "assets");
    if (!assets) {
        return ylt::unexpected<Error>{assets.error()};
    }
    return Release{input.version, input.platform, *runtime, *assets};
}

ylt::expected<Component, Error> parse_release_component(const Json::Value& object, const std::string_view context) {
    std::uint64_t size = 0;
    const auto url = string_member(object, "url");
    const auto sha = string_member(object, "sha256");
    if (!exact_json_fields(object, {"url", "sha256", "size"}) || !url || !sha ||
        !unsigned_member(object, "size", size)) {
        return failure(ErrorCode::Validation, "component fields are invalid", std::string(context));
    }
    // Values are checked once, with the whole release, by validate_release.
    return Component{*url, *sha, size};
}

ylt::expected<Release, Error> parse_release(const std::string_view json) {
    auto parsed = parse_json(json, "release manifest", "invalid release manifest JSON");
    if (!parsed) {
        return ylt::unexpected<Error>{parsed.error()};
    }
    const auto& value = *parsed;
    if (!exact_json_fields(value, {"version", "platform", "components"}) || !value["components"].isObject() ||
        !exact_json_fields(value["components"], {"runtime", "assets"})) {
        return failure(ErrorCode::Validation, "release manifest fields are invalid", "release");
    }
    const auto version = string_member(value, "version");
    const auto platform = string_member(value, "platform");
    if (!version || !platform) {
        return failure(ErrorCode::Validation, "release version or platform is invalid", "release");
    }
    auto runtime = parse_release_component(value["components"]["runtime"], "runtime");
    if (!runtime) {
        return ylt::unexpected<Error>{runtime.error()};
    }
    auto assets = parse_release_component(value["components"]["assets"], "assets");
    if (!assets) {
        return ylt::unexpected<Error>{assets.error()};
    }
    return validate_release(Release{*version, *platform, *runtime, *assets});
}

std::string metadata_text(const VersionMetadata& metadata) {
    Json::Value value(Json::objectValue);
    value["version"] = metadata.version;
    value["platform"] = metadata.platform;
    value["components"] = Json::objectValue;
    value["components"]["runtime"] = metadata.runtime;
    value["components"]["assets"] = metadata.assets;
    return json_string(value);
}

ylt::expected<VersionMetadata, Error> parse_metadata(const std::string_view text) {
    auto parsed = parse_json(text, "version metadata", "invalid version metadata JSON");
    if (!parsed) {
        return ylt::unexpected<Error>{parsed.error()};
    }
    const auto& value = *parsed;
    const auto& components = value["components"];
    const auto version = string_member(value, "version");
    const auto platform = string_member(value, "platform");
    const auto runtime = string_member(components, "runtime");
    const auto assets = string_member(components, "assets");
    if (!exact_json_fields(value, {"version", "platform", "components"}) || !components.isObject() ||
        !exact_json_fields(components, {"runtime", "assets"}) || !version || !platform || !parse_version(*version) ||
        !valid_token(*platform, 64) || !runtime || !assets || !valid_sha256(*runtime) || !valid_sha256(*assets)) {
        return failure(ErrorCode::Validation, "version metadata is invalid", std::string(layout::version_metadata));
    }
    return VersionMetadata{*version, *platform, lower(*runtime), lower(*assets)};
}

// ---------------------------------------------------------------------------
// Installed versions

std::filesystem::path versions_directory(const std::filesystem::path& root) {
    return root / layout::versions;
}

ylt::expected<void, Error> ensure_root(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return failure(ErrorCode::Validation, "installation root is not a directory", path_to_utf8(root));
    }
    return {};
}

ylt::expected<Installed, Error> read_installed(const std::filesystem::path& root, const std::string& version) {
    const auto directory = versions_directory(root) / version;
    auto text = read_bounded_file(directory / layout::version_metadata, "version metadata", max_metadata_bytes);
    if (!text) {
        return ylt::unexpected<Error>{text.error()};
    }
    auto metadata = parse_metadata(*text);
    if (!metadata) {
        return ylt::unexpected<Error>{metadata.error()};
    }
    if (metadata->version != version) {
        return failure(ErrorCode::Validation, "version metadata does not match its directory", version);
    }
    return Installed{std::move(*metadata), directory};
}

std::optional<std::string> read_pointer(const std::filesystem::path& root) {
    auto text = read_bounded_file(root / layout::current_pointer, "current version", 128);
    if (!text) {
        return std::nullopt;
    }
    auto version = std::move(*text);
    trim_line_end(version);
    if (!parse_version(version)) {
        return std::nullopt;
    }
    return version;
}

// A usable version has matching metadata and contains the program.
ylt::expected<Installed, Error> read_usable(const std::filesystem::path& root, const std::string& version) {
    auto installed = read_installed(root, version);
    if (!installed) {
        return installed;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(installed->directory / layout::program_name, error)) {
        return failure(ErrorCode::Validation, "installed version has no program", path_to_utf8(installed->directory));
    }
    return installed;
}

// Follows .beacon-current like the launcher does: when the pointer is damaged
// (for example by a power loss) or names an unusable directory, the newest
// usable version is current, so the installation can still be updated or
// rolled back.
ylt::expected<Installed, Error> load_current(const std::filesystem::path& root) {
    auto root_ok = ensure_root(root);
    if (!root_ok) {
        return ylt::unexpected<Error>{root_ok.error()};
    }
    if (const auto version = read_pointer(root)) {
        if (auto installed = read_usable(root, *version)) {
            return installed;
        }
    }
    std::optional<std::pair<Version, Installed>> newest;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(versions_directory(root), error)) {
        const auto name = path_to_utf8(entry.path().filename());
        const auto version = parse_version(name);
        if (!version || (newest && *version <= newest->first)) {
            continue;
        }
        if (auto installed = read_usable(root, name)) {
            newest.emplace(*version, std::move(*installed));
        }
    }
    if (!newest) {
        return failure(ErrorCode::Validation, "installation has no usable version; install a full package",
                       path_to_utf8(versions_directory(root)));
    }
    return std::move(newest->second);
}

ylt::expected<void, Error> select_version(const std::filesystem::path& root, const std::string& version) {
    auto selected = write_file_atomically(root / layout::current_pointer, version + "\n");
    if (selected) {
        // apply/rollback hold the exclusive installation lock, so these can only
        // be leftovers, including the fixed temporary name used by older builds.
        const auto prefix = std::string(layout::current_pointer) + ".tmp";
        std::error_code error;
        for (std::filesystem::directory_iterator entries(root, error);
             !error && entries != std::filesystem::directory_iterator{}; entries.increment(error)) {
            const auto name = path_to_utf8(entries->path().filename());
            if (name == prefix || name.starts_with(prefix + ".")) {
                std::error_code cleanup_error;
                if (entries->is_regular_file(cleanup_error))
                    std::filesystem::remove(entries->path(), cleanup_error);
            }
        }
    }
    return selected;
}

// Keeps the selected and the previously selected versions for rollback.
void prune_versions(const std::filesystem::path& root, const std::set<std::string>& keep) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(versions_directory(root), error)) {
        const auto name = path_to_utf8(entry.path().filename());
        if ((parse_version(name) && !keep.contains(name)) || name == layout::staging) {
            (void)remove_tree(entry.path());
        }
    }
}

// ---------------------------------------------------------------------------
// Preparing a version directory

ylt::expected<UpdatePlan, Error> make_plan(const VersionMetadata& installed, const Release& release) {
    const auto old_version = parse_version(installed.version);
    const auto new_version = parse_version(release.version);
    const bool runtime_changed = installed.runtime != release.runtime.sha256;
    const bool assets_changed = installed.assets != release.assets.sha256;
    if (installed.platform != release.platform) {
        return failure(ErrorCode::UnsupportedVersion, "release platform does not match installation", "platform");
    }
    if (*new_version < *old_version) {
        return failure(ErrorCode::UnsupportedVersion, "release is older than installation", "version");
    }
    if (*new_version == *old_version && (runtime_changed || assets_changed)) {
        return failure(ErrorCode::Validation, "components changed within an immutable version", "version");
    }
    return UpdatePlan{runtime_changed, assets_changed, *new_version != *old_version};
}

ylt::expected<void, Error> verify_package(const std::filesystem::path& path, const Component& component) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return failure(ErrorCode::Io, "cannot inspect package", path_to_utf8(path));
    }
    if (size != component.size) {
        return failure(ErrorCode::Validation, "package size does not match release", path_to_utf8(path));
    }
    auto digest = sha256_file(path, max_package_bytes);
    if (!digest) {
        return ylt::unexpected<Error>{digest.error()};
    }
    if (*digest != component.sha256) {
        return failure(ErrorCode::Validation, "package SHA-256 does not match release", path_to_utf8(path));
    }
    return {};
}

std::filesystem::perms archive_permissions(const unsigned int mode) {
    using std::filesystem::perms;
    // Installed files are always readable and writable by their owner.
    return static_cast<perms>(mode & 0755U) | perms::owner_read | perms::owner_write;
}

struct ArchiveReader {
    archive* value = nullptr;

    ~ArchiveReader() {
        if (value) {
            archive_read_free(value);
        }
    }
};

// Extracts one component into a fresh version directory. Entries must be plain
// files or directories inside the component, and their declared sizes are
// enforced against a total limit.
ylt::expected<void, Error> extract_archive(const std::filesystem::path& archive_path,
                                           const std::filesystem::path& destination, const ComponentKind kind) {
    ArchiveReader reader{archive_read_new()};
    if (!reader.value || archive_read_support_filter_none(reader.value) != ARCHIVE_OK ||
        archive_read_support_format_zip(reader.value) != ARCHIVE_OK ||
#if defined(_WIN32)
        archive_read_open_filename_w(reader.value, archive_path.c_str(), 1024 * 1024) != ARCHIVE_OK
#else
        archive_read_open_filename(reader.value, archive_path.c_str(), 1024 * 1024) != ARCHIVE_OK
#endif
    ) {
        return failure(ErrorCode::Validation, "package is not a readable ZIP archive", path_to_utf8(archive_path));
    }
    std::uint64_t total = 0;
    while (true) {
        archive_entry* entry = nullptr;
        const auto result = archive_read_next_header(reader.value, &entry);
        if (result == ARCHIVE_EOF) {
            break;
        }
        if (result != ARCHIVE_OK || !entry) {
            return failure(ErrorCode::Validation, "cannot read ZIP entry", path_to_utf8(archive_path));
        }
        const char* pathname = archive_entry_pathname_utf8(entry);
        if (!pathname) {
            pathname = archive_entry_pathname(entry);
        }
        if (!pathname) {
            return failure(ErrorCode::Validation, "ZIP entry path is invalid", path_to_utf8(archive_path));
        }
        const auto type = archive_entry_filetype(entry);
        const bool directory = type == AE_IFDIR;
        if (archive_entry_symlink(entry) || archive_entry_hardlink(entry) || (!directory && type != AE_IFREG)) {
            return failure(ErrorCode::Validation, "ZIP entry type is not allowed", pathname);
        }
        std::string path(pathname);
        if (directory && path.ends_with('/')) {
            path.pop_back();
        }
        if (!valid_relative_path(path) || !belongs_to(path, kind, directory)) {
            return failure(ErrorCode::Validation, "ZIP entry path is not allowed", pathname);
        }
        const auto target = destination / path_from_utf8(path);
        if (directory) {
            auto made = ensure_directory(target);
            if (!made) {
                return made;
            }
            continue;
        }
        if (!archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0) {
            return failure(ErrorCode::Validation, "ZIP entry size is missing", pathname);
        }
        const auto declared_size = static_cast<std::uint64_t>(archive_entry_size(entry));
        if (declared_size > max_component_bytes - total) {
            return failure(ErrorCode::SecurityLimit, "component contents exceed size limit", path);
        }
        total += declared_size;
        auto made = ensure_directory(target.parent_path());
        if (!made) {
            return made;
        }
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        std::uint64_t written = 0;
        while (output) {
            const void* block = nullptr;
            std::size_t length = 0;
            la_int64_t offset = 0;
            const auto read_result = archive_read_data_block(reader.value, &block, &length, &offset);
            if (read_result == ARCHIVE_EOF) {
                break;
            }
            if (read_result != ARCHIVE_OK || offset < 0 || static_cast<std::uint64_t>(offset) != written ||
                length > declared_size - written) {
                return failure(ErrorCode::Validation, "ZIP entry data is invalid", path);
            }
            output.write(static_cast<const char*>(block), static_cast<std::streamsize>(length));
            written += length;
        }
        output.close();
        if (!output) {
            return failure(ErrorCode::Io, "cannot write staged file", path);
        }
        if (written != declared_size) {
            return failure(ErrorCode::Validation, "ZIP entry size is invalid", path);
        }
        std::error_code error;
        std::filesystem::permissions(target, archive_permissions(static_cast<unsigned int>(archive_entry_mode(entry))),
                                     std::filesystem::perm_options::replace, error);
        if (error) {
            return failure(ErrorCode::Io, "cannot set staged file permissions", path);
        }
    }
    return {};
}

// Copies an unchanged component from the current version directory. Its top
// level entries decide the component, as in belongs_to().
ylt::expected<void, Error> copy_component(const std::filesystem::path& source, const std::filesystem::path& destination,
                                          const ComponentKind kind) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(source, error)) {
        const auto name = path_to_utf8(entry.path().filename());
        if (!belongs_to(name, kind, entry.is_directory(error))) {
            continue;
        }
        std::filesystem::copy(entry.path(), destination / entry.path().filename(),
                              std::filesystem::copy_options::recursive, error);
        if (error) {
            return failure(ErrorCode::Io, "cannot copy installed file", name);
        }
    }
    if (error) {
        return failure(ErrorCode::Io, "cannot read installed version", path_to_utf8(source));
    }
    return {};
}

ylt::expected<void, Error> prepare_component(const std::filesystem::path& staging, const Installed& current,
                                             const ComponentKind kind, const bool changed, const Component& component,
                                             const std::optional<std::filesystem::path>& archive) {
    if (!changed) {
        return copy_component(current.directory, staging, kind);
    }
    auto verified = verify_package(*archive, component);
    if (!verified) {
        return verified;
    }
    return extract_archive(*archive, staging, kind);
}

bool run_health_check(const HealthCheck& health_check, const std::filesystem::path& directory) {
    if (!health_check) {
        return true;
    }
    try {
        return health_check(directory);
    } catch (...) {
        return false;
    }
}

}  // namespace

ylt::expected<Release, Error> read_release(const std::string_view json) {
    return parse_release(json);
}

Engine::Engine(std::filesystem::path root) {
    std::error_code error;
    root_ = std::filesystem::absolute(std::move(root), error).lexically_normal();
}

ylt::expected<std::string, Error> Engine::current_version() const {
    auto current = load_current(root_);
    if (!current) {
        return ylt::unexpected<Error>{current.error()};
    }
    return current->metadata.version;
}

ylt::expected<UpdatePlan, Error> Engine::plan(const Release& release) const {
    auto checked = validate_release(release);
    if (!checked) {
        return ylt::unexpected<Error>{checked.error()};
    }
    auto current = load_current(root_);
    if (!current) {
        return ylt::unexpected<Error>{current.error()};
    }
    return make_plan(current->metadata, *checked);
}

ylt::expected<void, Error> Engine::apply(const Release& release, std::optional<std::filesystem::path> runtime_archive,
                                         std::optional<std::filesystem::path> assets_archive,
                                         const HealthCheck& health_check) {
    auto checked = validate_release(release);
    if (!checked) {
        return ylt::unexpected<Error>{checked.error()};
    }
    auto current = load_current(root_);
    if (!current) {
        return ylt::unexpected<Error>{current.error()};
    }
    const auto planned = make_plan(current->metadata, *checked);
    if (!planned) {
        return ylt::unexpected<Error>{planned.error()};
    }
    const auto plan = *planned;
    if (!plan.available()) {
        return {};
    }
    if (plan.runtime != runtime_archive.has_value() || plan.assets != assets_archive.has_value()) {
        return failure(ErrorCode::Validation, "archives must be supplied exactly for changed components", "apply");
    }

    // Nothing outside the staging directory changes until the version is
    // selected, so any failure before that only has to discard staging.
    const auto versions = versions_directory(root_);
    const auto staging = versions / layout::staging;
    auto prepared = ensure_directory(versions);
    if (prepared) {
        prepared = remove_tree(staging);
    }
    if (prepared) {
        prepared = ensure_directory(staging);
    }
    const auto discard = [&](Error error) -> ylt::expected<void, Error> {
        (void)remove_tree(staging);
        return ylt::unexpected<Error>{std::move(error)};
    };
    if (!prepared) {
        return discard(prepared.error());
    }
    prepared =
        prepare_component(staging, *current, ComponentKind::Runtime, plan.runtime, checked->runtime, runtime_archive);
    if (prepared) {
        prepared =
            prepare_component(staging, *current, ComponentKind::Assets, plan.assets, checked->assets, assets_archive);
    }
    if (!prepared) {
        return discard(prepared.error());
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(staging / layout::program_name, error))) {
        return discard(make_error(ErrorCode::Validation, "runtime does not contain the Beacon program",
                                  std::string(layout::program_name)));
    }
    const VersionMetadata metadata{checked->version, checked->platform, checked->runtime.sha256,
                                   checked->assets.sha256};
    prepared = write_file(staging / layout::version_metadata, metadata_text(metadata));
    if (!prepared) {
        return discard(prepared.error());
    }
    if (!run_health_check(health_check, staging)) {
        return discard(make_error(ErrorCode::Validation, "health check failed", "health"));
    }

    // A directory with the target name can only be left over from an update
    // interrupted before selection: the plan guarantees it is newer than the
    // current version.
    const auto target = versions / checked->version;
    prepared = sync_tree(staging);
    if (prepared) {
        prepared = remove_tree(target);
    }
    if (prepared) {
        prepared = rename_path(staging, target);
    }
    if (prepared) {
        prepared = sync_path(versions, true, true);
    }
    if (!prepared) {
        (void)remove_tree(target);
        return discard(prepared.error());
    }
    auto selected = select_version(root_, checked->version);
    if (!selected) {
        // The pointer may already have been replaced before its directory
        // flush failed. Keep the complete version valid in either case.
        return selected;
    }
    prune_versions(root_, {checked->version, current->metadata.version});
    return {};
}

ylt::expected<std::string, Error> Engine::rollback() {
    auto current = load_current(root_);
    if (!current) {
        return ylt::unexpected<Error>{current.error()};
    }
    const auto current_version = *parse_version(current->metadata.version);
    std::optional<std::pair<Version, std::string>> previous;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(versions_directory(root_), error)) {
        const auto name = path_to_utf8(entry.path().filename());
        const auto version = parse_version(name);
        if (!version || *version >= current_version || (previous && *version <= previous->first)) {
            continue;
        }
        auto installed = read_usable(root_, name);
        if (installed && installed->metadata.platform == current->metadata.platform) {
            previous.emplace(*version, name);
        }
    }
    if (error) {
        return failure(ErrorCode::Io, "cannot list installed versions", path_to_utf8(versions_directory(root_)));
    }
    if (!previous) {
        return failure(ErrorCode::Validation, "no earlier version is installed", "rollback");
    }
    auto selected = select_version(root_, previous->second);
    if (!selected) {
        return ylt::unexpected<Error>{selected.error()};
    }
    return previous->second;
}

}  // namespace beacon::update
