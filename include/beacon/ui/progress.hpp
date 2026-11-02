#pragma once

#include <beacon/app/runtime.hpp>
#include <beacon/ui/display.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace beacon {

struct ProgressStats {
    std::size_t completed = 0;
    std::size_t total = 0;

    [[nodiscard]] float ratio() const {
        return total == 0 ? 0.0F : static_cast<float>(completed) / static_cast<float>(total);
    }
};

struct ProgressViewModel {
    struct Collection {
        std::uint32_t parent;
        std::vector<std::size_t> members;
    };

    std::vector<std::uint32_t> goal_nodes;
    std::vector<std::string> labels;
    std::vector<std::vector<std::uint32_t>> main_groups;
    std::vector<std::vector<std::uint32_t>> overlay_groups;
    // Overlay groups with an item that can show a statistic line under its label.
    std::vector<bool> overlay_progress;
    std::vector<std::vector<Collection>> main_collections;
    std::vector<std::vector<std::uint32_t>> overlay_collection_starts;

    void update(const PublishedState& state);

private:
    std::shared_ptr<const CompiledTemplate> compiled_;
    std::shared_ptr<const Localization> localization_;
    std::shared_ptr<const Layout> layout_;
};

// Recipe-book pace for items that cycle through alternatives.
inline constexpr std::uint64_t item_cycle_ms = 1500;

// The nodes an item draws from, following each node's view.show: stages show the first unfinished child
// (the last once done), cycle shows the children that share the node's state in turn. Stages bring their
// own label, cycled children only their icon; missing parts come from the nearest ancestor.
struct ItemDisplay {
    // The deepest node reached; a statistic here is shown as a progress line.
    std::uint32_t value;
    std::uint32_t label;
    std::uint32_t icon;
    // The shown node changes with time, so the item needs a redraw every item_cycle_ms.
    bool cycling = false;
};
ItemDisplay item_display(std::uint32_t node, const CompiledTemplate& compiled, std::span<const RuleResult> results,
                         std::uint64_t now_ms = 0);
// The item and every node it may show in its place, so layouts can reserve room for the widest.
std::vector<std::uint32_t> item_variants(std::uint32_t node, const CompiledTemplate& compiled);
// Statistics show their count, or count and target, under the label.
bool shows_progress(std::uint32_t node, const CompiledTemplate& compiled);
// The leaf a manual click adjusts. Increments advance the active stage or mark a pending cycled child;
// decrements take back the active stage's progress, or reopen the previous stage when it has none, or
// unmark a completed cycled child. Empty when there is nothing to change.
std::optional<std::uint32_t> item_manual_node(std::uint32_t node, bool increment, const CompiledTemplate& compiled,
                                              std::span<const RuleResult> results, std::uint64_t now_ms = 0);
ProgressStats summarize_progress(const PublishedState& state, std::span<const std::uint32_t> goal_nodes);
bool is_complete(const PublishedState& state);

}  // namespace beacon
