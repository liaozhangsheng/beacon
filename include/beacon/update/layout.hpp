#pragma once

#include <filesystem>
#include <string_view>

// The installation layout shared by the launcher, Beacon and the updater.
//
//   <root>/beacon[.exe]              launcher; starts versions/<current>/beacon
//   <root>/.beacon-current           the selected version, e.g. "0.3.0\n"
//   <root>/versions/<version>/       one complete, immutable program version
//   <root>/templates/ config/        user data, never touched by updates
//   <root>/updater/                  standalone updater
namespace beacon::update::layout {

inline constexpr std::string_view current_pointer = ".beacon-current";
inline constexpr std::string_view versions = "versions";
inline constexpr std::string_view staging = ".staging";
inline constexpr std::string_view version_metadata = ".beacon-version.json";
inline constexpr std::string_view download_cache = ".beacon-downloads";
inline constexpr std::string_view failure_log = ".beacon-update-error.log";

#if defined(_WIN32)
inline constexpr std::string_view program_name = "beacon.exe";
#else
inline constexpr std::string_view program_name = "beacon";
#endif

inline std::filesystem::path updater_executable(const std::filesystem::path& root) {
#if defined(_WIN32)
    return root / "updater" / "beacon-updater.exe";
#else
    return root / "updater" / "beacon-updater";
#endif
}

// Returns the installation root for a program directory. A program inside
// <root>/versions/<name>/ belongs to <root>; any other directory, such as a
// development build tree, is its own installation root.
inline std::filesystem::path installation_root(const std::filesystem::path& program_directory) {
    auto directory = program_directory.lexically_normal();
    if (!directory.has_filename()) {
        directory = directory.parent_path();
    }
    const auto container = directory.parent_path();
    const auto root = container.parent_path();
    if (container.filename() == versions) {
        return root;
    }
    return directory;
}

}  // namespace beacon::update::layout
