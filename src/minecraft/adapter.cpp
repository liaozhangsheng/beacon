#include <beacon/minecraft/adapter.hpp>
#include "../core/json.hpp"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

namespace beacon {
namespace {

Error failure(ErrorCode code, std::string message, const std::filesystem::path& path) {
    return {.code = code, .message = std::move(message), .context = path_to_utf8(path.filename())};
}

ylt::expected<std::string, Error> read_consistent(const std::filesystem::path& path) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto before = file_stamp(path);
        if (!before)
            return ylt::unexpected<Error>{before.error()};
        if (!before->exists)
            return ylt::unexpected<Error>{failure(ErrorCode::Io, "Minecraft file is missing", path)};
        auto data = read_bounded_file(path, "Minecraft JSON");
        if (!data && data.error().code != ErrorCode::Io)
            return ylt::unexpected<Error>{std::move(data.error())};
        const auto after = file_stamp(path);
        if (data && after && *before == *after)
            return data;
        if (attempt == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ylt::unexpected<Error>{failure(ErrorCode::Io, "Minecraft file remained unstable", path)};
}

ylt::expected<Json::Value, Error> parse_file(const std::filesystem::path& path) {
    auto data = read_consistent(path);
    if (!data) {
        return ylt::unexpected<Error>{std::move(data.error())};
    }
    auto root = beacon::parse_json(*data, path_to_utf8(path.filename()), "invalid Minecraft JSON");
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    return std::move(*root);
}

std::filesystem::path save_root(const WorldLocation& world, const SaveLayout layout) {
    return layout == SaveLayout::PlayersSubdir ? world.path / "players" : world.path;
}

std::string world_storage_key(const std::filesystem::path& path) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : path_to_utf8(path)) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "world-" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    return std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end()).first == root.end();
}

bool path_is_within_root(const std::filesystem::path& root, const std::filesystem::path& candidate,
                         std::error_code& ec) {
    const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
    return !ec && path_is_within(root, canonical);
}

struct SourceCandidate {
    WorldLocation world;
    PlayerIdentity player;
    SaveLayout layout;
    std::filesystem::path stats;
    std::int64_t activity = 0;
};

bool newer(const SourceCandidate& candidate, const SourceCandidate& current) {
    if (candidate.activity != current.activity) {
        return current.activity < candidate.activity;
    }
    if (candidate.world.path != current.world.path) {
        return path_to_utf8(current.world.path) < path_to_utf8(candidate.world.path);
    }
    if (candidate.player.uuid != current.player.uuid) {
        return current.player.uuid < candidate.player.uuid;
    }
    return static_cast<int>(current.layout) < static_cast<int>(candidate.layout);
}

ylt::expected<std::optional<SourceCandidate>, Error> inspect_source(const WorldLocation& world, std::string uuid,
                                                                    SaveLayout layout,
                                                                    const std::filesystem::path& canonical_game_root) {
    const auto root = save_root(world, layout);
    const auto stats = root / "stats" / (uuid + ".json");
    const auto advancements = root / "advancements" / (uuid + ".json");
    std::error_code ec;
    const auto stats_status = std::filesystem::symlink_status(stats, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory)
            return std::optional<SourceCandidate>{};
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot inspect player stats", stats)};
    }
    if (!std::filesystem::exists(stats_status))
        return std::optional<SourceCandidate>{};
    if (std::filesystem::is_symlink(stats_status)) {
        return ylt::unexpected<Error>{
            failure(ErrorCode::Validation, "player stats must not be a symbolic link", stats)};
    }
    if (!std::filesystem::is_regular_file(stats_status))
        return std::optional<SourceCandidate>{};
    ec.clear();
    const auto stats_in_root = path_is_within_root(canonical_game_root, stats, ec);
    if (ec) {
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot resolve player stats path", stats)};
    }
    if (!stats_in_root) {
        return ylt::unexpected<Error>{
            failure(ErrorCode::Validation, "player stats path is outside game directory", stats)};
    }
    const auto stats_stamp = file_stamp(stats);
    if (!stats_stamp) {
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot inspect player stats", stats)};
    }
    if (!stats_stamp->exists) {
        return std::optional<SourceCandidate>{};
    }
    auto activity = stats_stamp->mtime;
    ec.clear();
    const auto advancements_status = std::filesystem::symlink_status(advancements, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory)
            ec.clear();
        else
            return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot inspect player advancements", advancements)};
    } else if (!std::filesystem::exists(advancements_status)) {
        ec.clear();
    } else if (std::filesystem::is_symlink(advancements_status) ||
               !std::filesystem::is_regular_file(advancements_status)) {
        return ylt::unexpected<Error>{
            failure(ErrorCode::Validation, "player advancements must be a regular file", advancements)};
    } else {
        const auto advancements_in_root = path_is_within_root(canonical_game_root, advancements, ec);
        if (ec) {
            return ylt::unexpected<Error>{
                failure(ErrorCode::Io, "cannot resolve player advancements path", advancements)};
        }
        if (!advancements_in_root) {
            return ylt::unexpected<Error>{
                failure(ErrorCode::Validation, "player advancements path is outside game directory", advancements)};
        }
        const auto advancements_stamp = file_stamp(advancements);
        if (!advancements_stamp) {
            return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot inspect player advancements", advancements)};
        }
        if (advancements_stamp->exists) {
            activity = std::max(activity, advancements_stamp->mtime);
        }
    }
    return std::optional<SourceCandidate>{SourceCandidate{
        .world = world,
        .player = {.uuid = std::move(uuid)},
        .layout = layout,
        .stats = stats,
        .activity = activity,
    }};
}

void add_warning(std::vector<Error>& warnings, Error error) {
    if (warnings.size() < max_diagnostics)
        warnings.push_back(std::move(error));
}

ylt::expected<std::vector<SourceCandidate>, Error> scan_world_layout(const WorldLocation& world, SaveLayout layout,
                                                                     const std::filesystem::path& canonical_game_root,
                                                                     std::vector<Error>& warnings) {
    const auto directory = save_root(world, layout) / "stats";
    std::error_code ec;
    std::filesystem::directory_iterator files(directory, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        return std::vector<SourceCandidate>{};
    }
    if (ec) {
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot scan player stats directory", directory)};
    }
    if (!path_is_within_root(canonical_game_root, directory, ec) || ec) {
        return ylt::unexpected<Error>{
            failure(ErrorCode::Validation, "player stats directory is outside game directory", directory)};
    }

    std::vector<SourceCandidate> candidates;
    for (; files != std::filesystem::directory_iterator{};) {
        const auto file_path = files->path();
        files.increment(ec);
        if (ec) {
            return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot scan player stats directory", directory)};
        }
        std::error_code file_ec;
        if (!std::filesystem::is_regular_file(file_path, file_ec)) {
            if (file_ec && file_ec != std::errc::no_such_file_or_directory) {
                add_warning(warnings, failure(ErrorCode::Io, "cannot inspect player stats", file_path));
            }
            continue;
        }
        if (file_path.extension() != ".json") {
            continue;
        }
        const auto uuid = file_path.stem().string();
        if (!valid_player_id(uuid)) {
            continue;
        }
        auto candidate = inspect_source(world, uuid, layout, canonical_game_root);
        if (!candidate) {
            add_warning(warnings, std::move(candidate.error()));
            continue;
        }
        if (*candidate) {
            candidates.push_back(std::move(**candidate));
        }
    }
    return candidates;
}

bool valid_json_key(std::string_view value) {
    return !value.empty() && value.size() <= max_string_bytes && valid_utf8(value);
}

}  // namespace

bool supports_template(const VersionProfile& profile, const CompiledTemplate& compiled) {
    if (!profile.data_version || *profile.data_version < compiled.minecraft.min_data_version ||
        *profile.data_version > compiled.minecraft.max_data_version) {
        return false;
    }
    for (const auto& node : compiled.graph.nodes) {
        if (node.op != RuleOp::Fact || node.fact_key == "clock/play_ticks" || node.fact_key.starts_with("stat/") ||
            node.fact_key.starts_with("manual/")) {
            continue;
        }
        if (node.fact_key.starts_with("adv/") || node.fact_key.starts_with("criterion/") ||
            node.fact_key.starts_with("recipe/")) {
            continue;
        }
        return false;
    }
    return true;
}

bool valid_player_id(std::string_view value) {
    return valid_path_component(value);
}

ylt::expected<VersionProfile, Error> SourceDiscovery::profile(const std::filesystem::path& stats, SaveLayout layout) {
    auto stamp = file_stamp(stats);
    if (!stamp)
        return ylt::unexpected<Error>{std::move(stamp.error())};
    auto data = read_consistent(stats);
    if (!data)
        return ylt::unexpected<Error>{std::move(data.error())};
    if (auto cached = profiles_.find(stats);
        cached != profiles_.end() && cached->second.stamp == *stamp && cached->second.data == *data) {
        cached->second.scan = scan_;
        auto profile = cached->second.profile;
        profile.layout = layout;
        return profile;
    }
    auto parsed = read_modern_stats(stats);
    if (!parsed)
        return ylt::unexpected<Error>{std::move(parsed.error())};
    auto after = file_stamp(stats);
    if (!after || *after != *stamp)
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "stats changed during version detection", stats)};
    VersionProfile profile{.data_version = parsed->data_version, .layout = layout};
    profiles_.insert_or_assign(stats, Entry{*stamp, profile, std::move(*data), scan_});
    return profile;
}

ylt::expected<ActiveMinecraftSource, Error> SourceDiscovery::scan(const std::filesystem::path& game_root,
                                                                  const ActiveMinecraftSource* current,
                                                                  const CompiledTemplate* compiled) {
    warnings.clear();
    ++scan_;
    const auto saves = game_root / "saves";
    std::error_code ec;
    const auto canonical_game_root = std::filesystem::weakly_canonical(game_root, ec);
    if (ec) {
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot resolve Minecraft game directory", game_root)};
    }
    ec.clear();
    if (!path_is_within_root(canonical_game_root, saves, ec) || ec) {
        return ylt::unexpected<Error>{
            failure(ErrorCode::Validation, "Minecraft saves directory is outside game directory", saves)};
    }
    std::filesystem::directory_iterator worlds(saves, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        return ylt::unexpected<Error>{failure(ErrorCode::NoSave, "Minecraft saves directory was not found", saves)};
    }
    if (ec)
        return ylt::unexpected<Error>{failure(ErrorCode::Io, "cannot scan Minecraft saves directory", saves)};
    std::optional<SourceCandidate> selected;
    std::optional<VersionProfile> selected_profile;
    std::optional<SourceCandidate> current_fallback;
    std::optional<VersionProfile> current_fallback_profile;
    std::vector<SourceCandidate> candidates;
    if (current) {
        auto candidate =
            inspect_source(current->world, current->player.uuid, current->profile.layout, canonical_game_root);
        if (!candidate) {
            add_warning(warnings, std::move(candidate.error()));
        } else if (*candidate) {
            auto detected = profile((**candidate).stats, current->profile.layout);
            if (detected && (!compiled || supports_template(*detected, *compiled))) {
                selected = std::move(**candidate);
                selected_profile = *detected;
            } else if (!detected) {
                // Preserve the last source only when no valid fallback is available.
                current_fallback = std::move(**candidate);
                current_fallback_profile = current->profile;
            }
        }
    }
    for (; worlds != std::filesystem::directory_iterator{}; worlds.increment(ec)) {
        if (ec)
            break;
        const auto world_path = worlds->path();
        std::error_code entry_ec;
        const auto world_in_root = path_is_within_root(canonical_game_root, world_path, entry_ec);
        if (entry_ec) {
            add_warning(warnings, failure(ErrorCode::Io, "cannot resolve Minecraft save", world_path));
            continue;
        }
        if (!world_in_root) {
            add_warning(warnings,
                        failure(ErrorCode::Validation, "Minecraft save is outside game directory", world_path));
            continue;
        }
        if (!worlds->is_directory(entry_ec)) {
            if (entry_ec && entry_ec != std::errc::no_such_file_or_directory)
                add_warning(warnings, failure(ErrorCode::Io, "cannot inspect Minecraft save", world_path));
            continue;
        }
        const WorldLocation world{
            {.display_name = path_to_utf8(world_path.filename()), .storage_key = world_storage_key(world_path)},
            world_path};
        for (const auto layout : {SaveLayout::WorldRoot, SaveLayout::PlayersSubdir}) {
            auto found = scan_world_layout(world, layout, canonical_game_root, warnings);
            if (!found) {
                add_warning(warnings, std::move(found.error()));
                continue;
            }
            for (auto& candidate : *found) {
                if (auto cached = profiles_.find(candidate.stats); cached != profiles_.end())
                    cached->second.scan = scan_;
                candidates.push_back(std::move(candidate));
            }
        }
    }
    if (ec)
        add_warning(warnings, failure(ErrorCode::Io, "cannot finish scanning Minecraft saves", saves));
    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        return newer(left, right);
    });
    for (const auto& candidate : candidates) {
        if (current_fallback && candidate.stats == current_fallback->stats)
            continue;
        if (selected && !newer(candidate, *selected))
            break;
        auto detected = profile(candidate.stats, candidate.layout);
        if (!detected) {
            add_warning(warnings, std::move(detected.error()));
            continue;
        }
        if (compiled && !supports_template(*detected, *compiled))
            continue;
        selected = candidate;
        selected_profile = std::move(*detected);
    }
    if (!selected && current_fallback) {
        selected = std::move(*current_fallback);
        selected_profile = std::move(*current_fallback_profile);
    }
    std::erase_if(profiles_, [this](const auto& entry) {
        return entry.second.scan != scan_;
    });
    if (!selected) {
        if (!warnings.empty())
            return ylt::unexpected<Error>{warnings.front()};
        return ylt::unexpected<Error>{failure(ErrorCode::NoSave, "no compatible player stats files were found", saves)};
    }
    auto instance = directory_identity(selected->world.path);
    if (!instance)
        return ylt::unexpected<Error>{std::move(instance.error())};
    return ActiveMinecraftSource{.world = std::move(selected->world),
                                 .player = std::move(selected->player),
                                 .profile = std::move(*selected_profile),
                                 .world_instance = std::move(*instance)};
}

ylt::expected<ActiveMinecraftSource, Error> detect_active_source(const std::filesystem::path& game_root,
                                                                 const ActiveMinecraftSource* current,
                                                                 const CompiledTemplate* compiled) {
    return SourceDiscovery{}.scan(game_root, current, compiled);
}

ylt::expected<MinecraftPaths, Error> player_data_paths(const WorldLocation& world, const PlayerIdentity& player,
                                                       const VersionProfile& profile) {
    if (!valid_player_id(player.uuid)) {
        return ylt::unexpected<Error>{{.code = ErrorCode::Validation,
                                       .message = "a safe confirmed player UUID is required",
                                       .context = "player"}};
    }
    const auto root = save_root(world, profile.layout);
    const auto player_file = player.uuid + ".json";
    return MinecraftPaths{
        .stats = root / "stats" / player_file,
        .advancements = root / "advancements" / player_file,
    };
}

ylt::expected<ModernStats, Error> read_modern_stats(const std::filesystem::path& path) {
    auto root = parse_file(path);
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    if (!root->isObject() || !root->isMember("DataVersion") || (*root)["DataVersion"].type() != Json::intValue ||
        !(*root)["stats"].isObject()) {
        return ylt::unexpected<Error>{failure(ErrorCode::Validation, "invalid Modern stats document", path)};
    }
    ModernStats result;
    result.data_version = (*root)["DataVersion"].asInt64();
    if (*result.data_version < 0) {
        return ylt::unexpected<Error>{failure(ErrorCode::Validation, "DataVersion must be non-negative", path)};
    }
    std::unordered_set<std::string> item_keys;
    const auto& categories = (*root)["stats"];
    for (auto category_it = categories.begin(); category_it != categories.end(); ++category_it) {
        const auto category = category_it.name();
        if (!valid_json_key(category)) {
            return ylt::unexpected<Error>{
                failure(ErrorCode::SecurityLimit, "stat category exceeds string limit", path)};
        }
        const auto& values = *category_it;
        if (!values.isObject()) {
            return ylt::unexpected<Error>{failure(ErrorCode::Validation, "stats category must be an object", path)};
        }
        const bool inventory_category = category == "minecraft:picked_up" || category == "minecraft:crafted" ||
                                        category == "minecraft:dropped" || category == "minecraft:used";
        for (auto value_it = values.begin(); value_it != values.end(); ++value_it) {
            const auto key = value_it.name();
            if (!valid_json_key(key)) {
                return ylt::unexpected<Error>{failure(ErrorCode::SecurityLimit, "stat key exceeds string limit", path)};
            }
            const auto& value = *value_it;
            if (value.type() != Json::intValue || value.asInt64() < 0) {
                return ylt::unexpected<Error>{
                    failure(ErrorCode::Validation, "stat value must be a non-negative int64", path)};
            }
            result.facts.emplace("stat/" + category + '/' + key, value.asInt64());
            if (inventory_category) {
                item_keys.insert(key);
            }
        }
    }

    // Derived, non-authoritative estimate: picked up + crafted - dropped - used.
    // It intentionally remains separate from the raw Minecraft statistics.
    const auto stat = [&](std::string_view category, const std::string& key) {
        const auto value = result.facts.find("stat/" + std::string(category) + '/' + key);
        return value == result.facts.end() ? std::int64_t{0} : value->second;
    };
    for (const auto& key : item_keys) {
        auto estimated = stat("minecraft:picked_up", key);
        const auto crafted = stat("minecraft:crafted", key);
        if (estimated > std::numeric_limits<std::int64_t>::max() - crafted) {
            estimated = std::numeric_limits<std::int64_t>::max();
        } else {
            estimated += crafted;
        }
        for (const auto category : {"minecraft:dropped", "minecraft:used"}) {
            const auto removed = stat(category, key);
            estimated = estimated > removed ? estimated - removed : 0;
        }
        result.facts.insert_or_assign("stat/minecraft:estimated/" + key, estimated);
    }

    auto playtime = result.facts.find("stat/minecraft:custom/minecraft:play_time");
    if (playtime == result.facts.end()) {
        playtime = result.facts.find("stat/minecraft:custom/minecraft:play_one_minute");
    }
    if (playtime == result.facts.end()) {
        return ylt::unexpected<Error>{failure(ErrorCode::Validation, "play time stat is missing", path)};
    }
    result.facts.emplace("clock/play_ticks", playtime->second);
    return result;
}

ylt::expected<Facts, Error> read_advancements(const std::filesystem::path& path) {
    auto root = parse_file(path);
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    if (!root->isObject()) {
        return ylt::unexpected<Error>{failure(ErrorCode::Validation, "advancements root must be an object", path)};
    }
    Facts facts;
    for (auto entry = root->begin(); entry != root->end(); ++entry) {
        const auto advancement = entry.name();
        const auto& value = *entry;
        if (advancement == "DataVersion") {
            if (!value.isInt64() || value.asInt64() < 0) {
                return ylt::unexpected<Error>{failure(ErrorCode::Validation, "invalid advancement metadata", path)};
            }
            continue;
        }
        if (!valid_json_key(advancement)) {
            return ylt::unexpected<Error>{
                failure(ErrorCode::SecurityLimit, "advancement key exceeds string limit", path)};
        }
        if (!value.isObject() || !value["criteria"].isObject() || !value["done"].isBool()) {
            return ylt::unexpected<Error>{failure(ErrorCode::Validation, "invalid advancement entry", path)};
        }
        facts.emplace("adv/" + advancement, value["done"].asBool() ? 1 : 0);
        const auto& criteria = value["criteria"];
        for (auto criterion_it = criteria.begin(); criterion_it != criteria.end(); ++criterion_it) {
            const auto criterion = criterion_it.name();
            if (!valid_json_key(criterion) || !criterion_it->isString()) {
                return ylt::unexpected<Error>{failure(ErrorCode::Validation, "invalid advancement criterion", path)};
            }
            const auto timestamp = criterion_it->asString();
            if (timestamp.size() > max_string_bytes || !valid_utf8(timestamp)) {
                return ylt::unexpected<Error>{failure(ErrorCode::Validation, "invalid advancement criterion", path)};
            }
            facts.emplace("criterion/" + advancement + '/' + criterion, 1);
        }
        constexpr std::string_view recipe_prefix = "minecraft:recipes/";
        if (advancement.starts_with(recipe_prefix) && value["done"].asBool()) {
            facts.emplace("recipe/minecraft:" + advancement.substr(recipe_prefix.size()), 1);
        }
    }
    return facts;
}

ylt::expected<Facts, Error> read_minecraft_facts(const WorldLocation& world, const PlayerIdentity& player,
                                                 const VersionProfile& profile,
                                                 std::optional<std::int64_t>* detected_data_version) {
    auto paths = player_data_paths(world, player, profile);
    if (!paths) {
        return ylt::unexpected<Error>{std::move(paths.error())};
    }

    // Share missing-file handling and pair consistency with background reads.
    auto read = MinecraftReader{}.read(*paths);
    if (!read)
        return ylt::unexpected<Error>{std::move(read.error())};
    auto& stats = **read;  // A fresh reader always returns data on success.
    if (detected_data_version)
        *detected_data_version = stats.data_version;
    return std::move(stats.facts);
}

void MinecraftReader::clear() {
    stats_stamp_.reset();
    advancements_stamp_.reset();
    stats_data_.reset();
    advancements_data_.reset();
}

ylt::expected<std::optional<ModernStats>, Error> MinecraftReader::read(const MinecraftPaths& paths) {
    if (paths_.stats != paths.stats || paths_.advancements != paths.advancements) {
        clear();
        paths_ = paths;
    }
    const auto fail = [this](Error error) -> ylt::expected<std::optional<ModernStats>, Error> {
        clear();  // Retry failures even when timestamps did not change.
        return ylt::unexpected<Error>{std::move(error)};
    };
    auto stats_stamp = file_stamp(paths.stats);
    auto advancements_stamp = file_stamp(paths.advancements);
    if (!stats_stamp)
        return fail(std::move(stats_stamp.error()));
    if (!advancements_stamp)
        return fail(std::move(advancements_stamp.error()));
    if (!stats_stamp->exists)
        return fail(failure(ErrorCode::Io, "player stats file is missing", paths.stats));
    auto stats_data = read_consistent(paths.stats);
    if (!stats_data)
        return fail(std::move(stats_data.error()));
    std::optional<std::string> advancements_data;
    if (advancements_stamp->exists) {
        auto data = read_consistent(paths.advancements);
        if (!data)
            return fail(std::move(data.error()));
        advancements_data = std::move(*data);
    }
    const bool stats_changed = !stats_data_ || *stats_data_ != *stats_data;
    const bool advancements_changed = advancements_data_ != advancements_data;
    if (!stats_changed && !advancements_changed)
        return std::optional<ModernStats>{};

    std::optional<ModernStats> stats;
    std::optional<Facts> advancements;
    if (stats_changed) {
        auto parsed = read_modern_stats(paths.stats);
        if (!parsed)
            return fail(std::move(parsed.error()));
        stats = std::move(*parsed);
    }
    if (advancements_changed) {
        if (advancements_stamp->exists) {
            auto parsed = read_advancements(paths.advancements);
            if (!parsed)
                return fail(std::move(parsed.error()));
            advancements = std::move(*parsed);
        } else {
            advancements.emplace();
        }
    }
    // Each reader validates its own file; also reject changes spanning the pair of reads.
    const auto stats_after = file_stamp(paths.stats);
    const auto advancements_after = file_stamp(paths.advancements);
    if (!stats_after || !advancements_after || *stats_after != *stats_stamp ||
        *advancements_after != *advancements_stamp)
        return fail(failure(ErrorCode::Io, "Minecraft files changed during refresh", paths.stats));
    if (stats)
        stats_ = std::move(*stats);
    if (advancements)
        advancements_ = std::move(*advancements);
    stats_stamp_ = *stats_stamp;
    advancements_stamp_ = *advancements_stamp;
    stats_data_ = std::move(*stats_data);
    advancements_data_ = std::move(advancements_data);
    ModernStats combined = stats_;
    combined.facts.insert(advancements_.begin(), advancements_.end());
    return std::optional<ModernStats>{std::move(combined)};
}

}  // namespace beacon
