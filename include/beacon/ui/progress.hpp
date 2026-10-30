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
    // Groups of stat/ counters or ordered stat stages, kept in fixed slots.
    std::vector<bool> main_stats;
    std::vector<bool> overlay_stats;
    std::vector<std::vector<Collection>> main_collections;
    std::vector<std::vector<std::uint32_t>> overlay_collection_starts;

    void update(const PublishedState& state);

private:
    std::shared_ptr<const CompiledTemplate> compiled_;
    std::shared_ptr<const Localization> localization_;
    std::shared_ptr<const Layout> layout_;
};

std::uint32_t stat_display_node(std::uint32_t node, const CompiledTemplate& compiled,
                                std::span<const RuleResult> results);
// The stage a manual click adjusts: increments advance the active stage; decrements take back
// its progress, or reopen the previous stage when it has none. Empty when there is nothing to change.
std::optional<std::uint32_t> stat_manual_node(std::uint32_t node, bool increment, const CompiledTemplate& compiled,
                                              std::span<const RuleResult> results);
ProgressStats summarize_progress(const PublishedState& state, std::span<const std::uint32_t> goal_nodes);
bool is_complete(const PublishedState& state);

}  // namespace beacon
