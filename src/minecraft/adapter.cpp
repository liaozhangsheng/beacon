#include <beacon/minecraft/adapter.hpp>
#include "../core/json.hpp"

#include <json/json.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace beacon {
namespace {

ylt::expected<std::string, Error> read_minecraft_file(const std::filesystem::path& path) {
    return read_bounded_file(path, "Minecraft JSON");
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

std::optional<SourceCandidate> inspect_source(const WorldLocation& world, std::string uuid, SaveLayout layout) {
    const auto root = save_root(world, layout);
    const auto stats = root / "stats" / (uuid + ".json");
    const auto stats_stamp = file_stamp(stats);
    if (!stats_stamp.exists)
        return std::nullopt;
    const auto advancements_stamp = file_stamp(root / "advancements" / (uuid + ".json"));
    return SourceCandidate{.world = world,
                           .player = {.uuid = std::move(uuid)},
                           .layout = layout,
                           .stats = stats,
                           .activity = std::max(stats_stamp.mtime, advancements_stamp.mtime)};
}

ylt::expected<std::vector<SourceCandidate>, Error> scan_world_layout(const WorldLocation& world, SaveLayout layout) {
    const auto directory = save_root(world, layout) / "stats";
    std::error_code ec;
    std::filesystem::directory_iterator files(directory, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return std::vector<SourceCandidate>{};
    if (ec)
        return ylt::unexpected<Error>{file_error(ErrorCode::Io, "cannot scan player stats directory", directory)};
    std::vector<SourceCandidate> candidates;
    for (const auto& file : files) {
        const auto uuid = path_to_utf8(file.path().stem());
        if (file.path().extension() != ".json" || !valid_player_id(uuid))
            continue;
        if (auto candidate = inspect_source(world, uuid, layout))
            candidates.push_back(std::move(*candidate));
    }
    return candidates;
}

// path only names the file in errors.
ylt::expected<ModernStats, Error> parse_modern_stats(const std::string_view data, const std::filesystem::path& path) {
    auto root = beacon::parse_json(data, path_to_utf8(path.filename()), "invalid Minecraft JSON");
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    if (!root->isObject() || !root->isMember("DataVersion") || !(*root)["DataVersion"].isInt64() ||
        !(*root)["stats"].isObject()) {
        return ylt::unexpected<Error>{file_error(ErrorCode::Validation, "invalid Modern stats document", path)};
    }
    ModernStats result;
    result.data_version = (*root)["DataVersion"].asInt64();
    if (*result.data_version < 0) {
        return ylt::unexpected<Error>{file_error(ErrorCode::Validation, "DataVersion must be non-negative", path)};
    }
    std::unordered_set<std::string> item_keys;
    const auto& categories = (*root)["stats"];
    for (auto category_it = categories.begin(); category_it != categories.end(); ++category_it) {
        const auto category = category_it.name();
        const auto& values = *category_it;
        if (!values.isObject()) {
            return ylt::unexpected<Error>{file_error(ErrorCode::Validation, "stats category must be an object", path)};
        }
        const bool inventory_category = category == "minecraft:picked_up" || category == "minecraft:crafted" ||
                                        category == "minecraft:dropped" || category == "minecraft:used";
        for (auto value_it = values.begin(); value_it != values.end(); ++value_it) {
            const auto key = value_it.name();
            const auto& value = *value_it;
            if (!value.isInt64() || value.asInt64() < 0) {
                return ylt::unexpected<Error>{
                    file_error(ErrorCode::Validation, "stat value must be a non-negative int64", path)};
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
        const auto estimated = stat("minecraft:picked_up", key) + stat("minecraft:crafted", key) -
                               stat("minecraft:dropped", key) - stat("minecraft:used", key);
        result.facts.insert_or_assign("stat/minecraft:estimated/" + key, std::max<std::int64_t>(0, estimated));
    }

    auto playtime = result.facts.find("stat/minecraft:custom/minecraft:play_time");
    if (playtime == result.facts.end()) {
        playtime = result.facts.find("stat/minecraft:custom/minecraft:play_one_minute");
    }
    if (playtime == result.facts.end()) {
        return ylt::unexpected<Error>{file_error(ErrorCode::Validation, "play time stat is missing", path)};
    }
    result.facts.emplace("clock/play_ticks", playtime->second);
    return result;
}

ylt::expected<Facts, Error> parse_advancements(const std::string_view data, const std::filesystem::path& path) {
    auto root = beacon::parse_json(data, path_to_utf8(path.filename()), "invalid Minecraft JSON");
    if (!root) {
        return ylt::unexpected<Error>{std::move(root.error())};
    }
    if (!root->isObject()) {
        return ylt::unexpected<Error>{file_error(ErrorCode::Validation, "advancements root must be an object", path)};
    }
    Facts facts;
    for (auto entry = root->begin(); entry != root->end(); ++entry) {
        const auto advancement = entry.name();
        const auto& value = *entry;
        if (advancement == "DataVersion")
            continue;
        if (!value.isObject() || !value["criteria"].isObject() || !value["done"].isBool()) {
            return ylt::unexpected<Error>{file_error(ErrorCode::Validation, "invalid advancement entry", path)};
        }
        facts.emplace("adv/" + advancement, value["done"].asBool() ? 1 : 0);
        const auto& criteria = value["criteria"];
        for (auto criterion_it = criteria.begin(); criterion_it != criteria.end(); ++criterion_it) {
            facts.emplace("criterion/" + advancement + '/' + criterion_it.name(), 1);
        }
        constexpr std::string_view recipe_prefix = "minecraft:recipes/";
        if (advancement.starts_with(recipe_prefix) && value["done"].asBool()) {
            facts.emplace("recipe/minecraft:" + advancement.substr(recipe_prefix.size()), 1);
        }
    }
    return facts;
}

}  // namespace

bool supports_template(const VersionProfile& profile, const CompiledTemplate& compiled) {
    if (!profile.data_version || *profile.data_version < compiled.minecraft.min_data_version ||
        *profile.data_version > compiled.minecraft.max_data_version) {
        return false;
    }
    for (const auto& node : compiled.graph.nodes) {
        const auto& key = node.fact_key;
        const bool readable = key == "clock/play_ticks" || key.starts_with("stat/") || key.starts_with("manual/") ||
                              key.starts_with("adv/") || key.starts_with("criterion/") || key.starts_with("recipe/");
        if (node.op == RuleOp::Fact && !readable)
            return false;
    }
    return true;
}

bool valid_player_id(std::string_view value) {
    return valid_path_component(value);
}

ylt::expected<VersionProfile, Error> SourceDiscovery::profile(const std::filesystem::path& stats, SaveLayout layout) {
    auto data = read_minecraft_file(stats);
    if (!data)
        return ylt::unexpected<Error>{std::move(data.error())};
    auto& cached = profiles_[stats];
    cached.scan = scan_;
    if (cached.data != *data) {
        auto parsed = parse_modern_stats(*data, stats);
        if (!parsed) {
            profiles_.erase(stats);
            return ylt::unexpected<Error>{std::move(parsed.error())};
        }
        cached.data = std::move(*data);
        cached.data_version = parsed->data_version;
    }
    return VersionProfile{.data_version = cached.data_version, .layout = layout};
}

ylt::expected<ActiveMinecraftSource, Error> SourceDiscovery::scan(const std::filesystem::path& game_root,
                                                                  const ActiveMinecraftSource* current,
                                                                  const CompiledTemplate* compiled) {
    warnings.clear();
    ++scan_;
    const auto saves = game_root / "saves";
    std::error_code ec;
    std::filesystem::directory_iterator worlds(saves, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        return ylt::unexpected<Error>{file_error(ErrorCode::NoSave, "Minecraft saves directory was not found", saves)};
    }
    if (ec)
        return ylt::unexpected<Error>{file_error(ErrorCode::Io, "cannot scan Minecraft saves directory", saves)};
    std::optional<SourceCandidate> selected;
    std::optional<VersionProfile> selected_profile;
    std::optional<SourceCandidate> current_fallback;
    std::optional<VersionProfile> current_fallback_profile;
    std::vector<SourceCandidate> candidates;
    if (current) {
        if (auto candidate = inspect_source(current->world, current->player.uuid, current->profile.layout)) {
            auto detected = profile(candidate->stats, current->profile.layout);
            if (detected && (!compiled || supports_template(*detected, *compiled))) {
                selected = std::move(*candidate);
                selected_profile = *detected;
            } else if (!detected) {
                // Preserve the last source only when no valid fallback is available.
                current_fallback = std::move(*candidate);
                current_fallback_profile = current->profile;
            }
        }
    }
    for (; worlds != std::filesystem::directory_iterator{}; worlds.increment(ec)) {
        if (ec)
            break;
        const auto world_path = worlds->path();
        std::error_code entry_ec;
        if (!worlds->is_directory(entry_ec))
            continue;
        const WorldLocation world{
            {.display_name = path_to_utf8(world_path.filename()), .storage_key = world_storage_key(world_path)},
            world_path};
        for (const auto layout : {SaveLayout::WorldRoot, SaveLayout::PlayersSubdir}) {
            auto found = scan_world_layout(world, layout);
            if (!found) {
                warnings.push_back(std::move(found.error()));
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
        warnings.push_back(file_error(ErrorCode::Io, "cannot finish scanning Minecraft saves", saves));
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
            warnings.push_back(std::move(detected.error()));
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
        return ylt::unexpected<Error>{
            file_error(ErrorCode::NoSave, "no compatible player stats files were found", saves)};
    }
    return ActiveMinecraftSource{.world = std::move(selected->world),
                                 .player = std::move(selected->player),
                                 .profile = std::move(*selected_profile)};
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

void MinecraftReader::clear() {
    stats_data_.reset();
    advancements_data_.reset();
    // A cleared reader must not merge the facts it last read into the next file pair.
    stats_ = {};
    advancements_.clear();
}

ylt::expected<std::optional<ModernStats>, Error> MinecraftReader::read(const MinecraftPaths& paths) {
    if (paths_.stats != paths.stats || paths_.advancements != paths.advancements) {
        clear();
        paths_ = paths;
    }
    const auto fail = [this](Error error) -> ylt::expected<std::optional<ModernStats>, Error> {
        clear();  // Retry failures even when the files did not change.
        return ylt::unexpected<Error>{std::move(error)};
    };
    auto stats_data = read_minecraft_file(paths.stats);
    if (!stats_data)
        return fail(std::move(stats_data.error()));
    std::optional<std::string> advancements_data;
    if (file_stamp(paths.advancements).exists) {
        auto data = read_minecraft_file(paths.advancements);
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
        auto parsed = parse_modern_stats(*stats_data, paths.stats);
        if (!parsed)
            return fail(std::move(parsed.error()));
        stats = std::move(*parsed);
    }
    if (advancements_changed) {
        if (advancements_data) {
            auto parsed = parse_advancements(*advancements_data, paths.advancements);
            if (!parsed)
                return fail(std::move(parsed.error()));
            advancements = std::move(*parsed);
        } else {
            advancements.emplace();
        }
    }
    if (stats)
        stats_ = std::move(*stats);
    if (advancements)
        advancements_ = std::move(*advancements);
    stats_data_ = std::move(*stats_data);
    advancements_data_ = std::move(advancements_data);
    ModernStats combined = stats_;
    combined.facts.insert(advancements_.begin(), advancements_.end());
    return std::optional<ModernStats>{std::move(combined)};
}

}  // namespace beacon
