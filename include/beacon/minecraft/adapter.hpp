#pragma once

#include <beacon/core/model.hpp>
#include <beacon/io/file.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace beacon {

enum class SaveLayout { WorldRoot, PlayersSubdir };

struct VersionProfile {
    std::optional<std::int64_t> data_version;
    SaveLayout layout = SaveLayout::WorldRoot;
};

struct WorldLocation {
    WorldIdentity identity;
    std::filesystem::path path;
};

struct MinecraftPaths {
    std::filesystem::path stats;
    std::filesystem::path advancements;
};

struct ModernStats {
    Facts facts;
    std::optional<std::int64_t> data_version;
};

struct ActiveMinecraftSource {
    WorldLocation world;
    PlayerIdentity player;
    VersionProfile profile;
    std::string world_instance;
};

// One cache entry per candidate file; only successful validation is cached.
class SourceDiscovery {
public:
    ylt::expected<ActiveMinecraftSource, Error> scan(const std::filesystem::path& game_root,
                                                     const ActiveMinecraftSource* current,
                                                     const CompiledTemplate* compiled);
    std::vector<Error> warnings;

private:
    struct Entry {
        FileStamp stamp;
        VersionProfile profile;
        std::string data;
        std::uint64_t scan = 0;
    };
    ylt::expected<VersionProfile, Error> profile(const std::filesystem::path& stats, SaveLayout layout);
    std::unordered_map<std::filesystem::path, Entry> profiles_;
    std::uint64_t scan_ = 0;
};

// The active player's two documents are independently cached and published together.
class MinecraftReader {
public:
    ylt::expected<std::optional<ModernStats>, Error> read(const MinecraftPaths& paths);
    void clear();

private:
    MinecraftPaths paths_;
    std::optional<FileStamp> stats_stamp_;
    std::optional<FileStamp> advancements_stamp_;
    std::optional<std::string> stats_data_;
    std::optional<std::string> advancements_data_;
    ModernStats stats_;
    Facts advancements_;
};

bool supports_template(const VersionProfile& profile, const CompiledTemplate& compiled);
bool valid_player_id(std::string_view value);
ylt::expected<ActiveMinecraftSource, Error> detect_active_source(const std::filesystem::path& game_root,
                                                                 const ActiveMinecraftSource* current = nullptr,
                                                                 const CompiledTemplate* compiled = nullptr);
ylt::expected<MinecraftPaths, Error> player_data_paths(const WorldLocation& world, const PlayerIdentity& player,
                                                       const VersionProfile& profile);
ylt::expected<ModernStats, Error> read_modern_stats(const std::filesystem::path& path);
ylt::expected<Facts, Error> read_advancements(const std::filesystem::path& path);
ylt::expected<Facts, Error> read_minecraft_facts(const WorldLocation& world, const PlayerIdentity& player,
                                                 const VersionProfile& profile,
                                                 std::optional<std::int64_t>* detected_data_version = nullptr);

}  // namespace beacon
