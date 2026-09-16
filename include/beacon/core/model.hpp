#pragma once

#include <ylt/util/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace beacon {

inline constexpr std::size_t max_json_depth = 128;
inline constexpr std::size_t max_json_bytes = 16U * 1024U * 1024U;
inline constexpr std::size_t max_rule_nodes = 10'000;
inline constexpr std::size_t max_string_bytes = 4'096;
inline constexpr std::size_t max_diagnostics = 100;

enum class ErrorCode {
    Io,
    NoSave,
    Parse,
    Validation,
    UnsupportedVersion,
    SecurityLimit,
    Internal,
};

struct Error {
    ErrorCode code;
    std::string message;
    std::string context;

    bool operator==(const Error&) const = default;
};

using Facts = std::unordered_map<std::string, std::int64_t>;

struct PlayerIdentity {
    std::string uuid;
};

struct WorldIdentity {
    std::string display_name;
    std::string storage_key;
};

struct RunIdentity {
    WorldIdentity world;
    PlayerIdentity player;
};

struct MinecraftCompatibility {
    std::int64_t min_data_version = 0;
    std::int64_t max_data_version = 0;

    bool operator==(const MinecraftCompatibility&) const = default;
};

enum class RuleOp : std::uint8_t {
    Fact,
    Ref,
    All,
    Any,
    Count,
    Prefix,
};

struct Presentation {
    enum class FrameType : std::uint8_t { None, Task, Goal, Challenge };

    std::string localization_key;
    std::string icon_path;
    FrameType frame_type = FrameType::None;
    std::string frame_obtained_path;
    std::string frame_unobtained_path;

    bool operator==(const Presentation&) const = default;
};

struct RuleNode {
    RuleOp op = RuleOp::Fact;
    std::string id;
    std::string fact_key;
    std::int64_t target = 1;
    std::vector<std::uint32_t> children;
    std::uint32_t reference_node = std::numeric_limits<std::uint32_t>::max();

    bool operator==(const RuleNode&) const = default;
};

struct RuleGraph {
    std::vector<RuleNode> nodes;
    std::vector<std::uint32_t> evaluation_order;
    std::unordered_map<std::string, std::uint32_t> index_by_id;
};

struct CompiledTemplate {
    std::string template_name_key;
    MinecraftCompatibility minecraft;
    RuleGraph graph;
    std::vector<std::optional<Presentation>> presentation_by_node;
    std::uint32_t completion_node = std::numeric_limits<std::uint32_t>::max();

    // Node order matters because snapshots and manual progress use node indices.
    [[nodiscard]] bool same_rules(const CompiledTemplate& other) const {
        return this == &other || (minecraft == other.minecraft && completion_node == other.completion_node &&
                                  graph.nodes == other.graph.nodes);
    }
};

using TemplateErrors = std::vector<Error>;

bool valid_utf8(std::string_view text);
bool valid_icon_reference(std::string_view value);
bool valid_language(std::string_view value);
bool valid_path_component(std::string_view value);

struct RuleResult {
    std::int64_t value = 0;
    std::int64_t target = 0;
    bool done = false;
    std::uint32_t active_child = std::numeric_limits<std::uint32_t>::max();

    bool operator==(const RuleResult&) const = default;
};

using RuleResults = std::vector<RuleResult>;

struct Snapshot {
    std::uint64_t revision = 0;
    std::int64_t updated_at = 0;
    std::uint64_t run_epoch = 0;
    RuleResults results;
    std::int64_t play_ticks = 0;
};

struct ManualProgress {
    std::unordered_set<std::uint32_t> completed;
    std::unordered_map<std::uint32_t, std::int64_t> stat_deltas;
};

struct PublishContext {
    std::uint64_t run_epoch = 0;
    std::int64_t play_ticks = 0;
};

ylt::expected<CompiledTemplate, TemplateErrors> compile_template_json(std::string_view json);
ylt::expected<RuleResults, Error> evaluate(const CompiledTemplate& compiled, const Facts& facts);
ylt::expected<void, Error> apply_manual_progress(const CompiledTemplate& compiled, RuleResults& results,
                                                 const ManualProgress& manual);
ylt::expected<void, Error> apply_manual_operation(const CompiledTemplate& compiled, RuleResults& results,
                                                  ManualProgress& manual, std::uint32_t node, bool increment);
ylt::expected<Snapshot, Error> publish(const CompiledTemplate& compiled, RuleResults results,
                                       const PublishContext& context, const Snapshot* previous);

}  // namespace beacon
