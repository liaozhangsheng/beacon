#include <beacon/ui/progress.hpp>

#include <algorithm>
#include <iterator>

namespace beacon {

bool is_complete(const PublishedState& state) {
    return state.data_status == DataStatus::Ready && state.compiled &&
           state.compiled->completion_node < state.snapshot.results.size() &&
           state.snapshot.results[state.compiled->completion_node].done;
}

void ProgressViewModel::update(const PublishedState& state) {
    if (compiled_ == state.compiled && localization_ == state.localization && layout_ == state.layout) {
        return;
    }
    const bool rebuild_groups = compiled_ != state.compiled || layout_ != state.layout;
    const auto result_count = state.snapshot.results.size();
    compiled_ = state.compiled;
    localization_ = state.localization;
    layout_ = state.layout;

    labels.clear();
    labels.reserve(state.compiled->graph.nodes.size());
    for (std::uint32_t node = 0; node < state.compiled->graph.nodes.size(); ++node)
        labels.push_back(display_label(node, *state.compiled, *state.localization));
    if (!rebuild_groups)
        return;

    goal_nodes.clear();
    main_groups.clear();
    overlay_groups.clear();
    for (std::uint32_t node = 0; node < state.compiled->graph.nodes.size(); ++node) {
        const auto& rule = state.compiled->graph.nodes[node];
        if (rule.id.starts_with("goal/"))
            goal_nodes.push_back(node);
    }
    const auto resolve_groups = [&](const auto& groups, auto& resolved) {
        resolved.reserve(groups.size());
        for (const auto& group : groups) {
            resolved.push_back(resolve_layout_nodes(group, *state.compiled, result_count));
        }
    };
    resolve_groups(state.layout->main, main_groups);
    resolve_groups(state.layout->overlay, overlay_groups);
    const auto group_progress = [&](const std::vector<std::uint32_t>& nodes) {
        return std::ranges::any_of(nodes, [&](const auto node) {
            return std::ranges::any_of(item_variants(node, *state.compiled), [&](const auto variant) {
                return shows_progress(variant, *state.compiled);
            });
        });
    };
    overlay_progress.clear();
    for (const auto& nodes : overlay_groups)
        overlay_progress.push_back(group_progress(nodes));

    const auto collections = [&](const LayoutGroup& group, const std::vector<std::uint32_t>& nodes) {
        std::vector<Collection> resolved;
        if (group.source != LayoutSource::Collections) {
            return resolved;
        }
        std::vector<std::size_t> positions(state.compiled->graph.nodes.size(), nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            positions[nodes[index]] = index;
        }
        for (const auto& id : group.nodes) {
            const auto parent = state.compiled->graph.index_by_id.at(id);
            Collection collection{parent, {}};
            for (const auto child : state.compiled->graph.nodes[parent].children) {
                if (positions[child] != nodes.size()) {
                    collection.members.push_back(positions[child]);
                }
            }
            if (!collection.members.empty()) {
                resolved.push_back(std::move(collection));
            }
        }
        return resolved;
    };
    main_collections.clear();
    for (std::size_t index = 0; index < main_groups.size(); ++index) {
        main_collections.push_back(collections(state.layout->main[index], main_groups[index]));
    }
    overlay_collection_starts.assign(overlay_groups.size(), {});
    for (std::size_t index = 0; index < overlay_groups.size(); ++index) {
        for (const auto& collection : collections(state.layout->overlay[index], overlay_groups[index])) {
            overlay_collection_starts[index].push_back(overlay_groups[index][collection.members.front()]);
        }
    }
}

namespace {

bool has_view(const std::uint32_t node, const CompiledTemplate& compiled) {
    return compiled.presentation_by_node[node].has_value();
}

bool has_icon(const std::uint32_t node, const CompiledTemplate& compiled) {
    const auto& view = compiled.presentation_by_node[node];
    return view && !view->icon_path.empty();
}

// Nodes without a view still walk a prefix chain, since stages are what a prefix means.
Presentation::Show item_show(const std::uint32_t node, const CompiledTemplate& compiled) {
    if (const auto& view = compiled.presentation_by_node[node])
        return view->show;
    return compiled.graph.nodes[node].op == RuleOp::Prefix ? Presentation::Show::Stages : Presentation::Show::Self;
}

// The children a cycling node shows in turn: those that agree with it, so a pending node shows
// what is left and a finished one shows what finished it.
std::vector<std::uint32_t> alternatives(const std::uint32_t node, const CompiledTemplate& compiled,
                                        const std::span<const RuleResult> results) {
    const auto& children = compiled.graph.nodes[node].children;
    std::vector<std::uint32_t> matching;
    for (const auto child : children) {
        if (child < results.size() && results[child].done == results[node].done)
            matching.push_back(child);
    }
    return matching.empty() ? children : matching;
}

struct Step {
    std::uint32_t node;
    bool stage = false;
    bool cycling = false;
};

std::optional<Step> shown_child(const std::uint32_t node, const CompiledTemplate& compiled,
                                const std::span<const RuleResult> results, const std::uint64_t now_ms) {
    const auto& rule = compiled.graph.nodes[node];
    if (node >= results.size() || rule.children.empty())
        return std::nullopt;
    switch (item_show(node, compiled)) {
        case Presentation::Show::Self:
            return std::nullopt;
        case Presentation::Show::Stages:
            if (results[node].done)
                return Step{rule.children.back(), true};
            if (rule.op == RuleOp::Prefix) {
                // Unevaluated results have no active stage yet.
                if (results[node].active_child < results.size())
                    return Step{results[node].active_child, true};
                return std::nullopt;
            }
            for (const auto child : rule.children) {
                if (child < results.size() && !results[child].done)
                    return Step{child, true};
            }
            return std::nullopt;
        case Presentation::Show::Cycle: {
            const auto choices = alternatives(node, compiled, results);
            return Step{choices[(now_ms / item_cycle_ms) % choices.size()], false, choices.size() > 1};
        }
    }
    return std::nullopt;
}

}  // namespace

ItemDisplay item_display(const std::uint32_t node, const CompiledTemplate& compiled,
                         const std::span<const RuleResult> results, const std::uint64_t now_ms) {
    ItemDisplay shown{.value = node, .label = node, .icon = node};
    const auto icon_rank = [&](const std::uint32_t candidate) {
        return has_icon(candidate, compiled) ? 2 : has_view(candidate, compiled) ? 1 : 0;
    };
    while (const auto step = shown_child(shown.value, compiled, results, now_ms)) {
        if (has_view(step->node, compiled) && (step->stage || !has_view(shown.label, compiled)))
            shown.label = step->node;
        if (icon_rank(step->node) >= icon_rank(shown.icon))
            shown.icon = step->node;
        shown.value = step->node;
        shown.cycling = shown.cycling || step->cycling;
    }
    return shown;
}

std::vector<std::uint32_t> item_variants(const std::uint32_t node, const CompiledTemplate& compiled) {
    std::vector<std::uint32_t> variants;
    std::vector<std::uint32_t> pending{node};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        variants.push_back(current);
        if (item_show(current, compiled) != Presentation::Show::Self) {
            const auto& children = compiled.graph.nodes[current].children;
            pending.insert(pending.end(), children.rbegin(), children.rend());
        }
    }
    return variants;
}

bool shows_progress(const std::uint32_t node, const CompiledTemplate& compiled) {
    const auto& rule = compiled.graph.nodes[node];
    return rule.op == RuleOp::Fact && rule.fact_key.starts_with("stat/");
}

std::optional<std::uint32_t> item_manual_node(const std::uint32_t node, const bool increment,
                                              const CompiledTemplate& compiled,
                                              const std::span<const RuleResult> results, const std::uint64_t now_ms) {
    const auto& rule = compiled.graph.nodes[node];
    if (item_show(node, compiled) == Presentation::Show::Stages) {
        const auto step = shown_child(node, compiled, results, now_ms);
        if (!step)
            return std::nullopt;
        if (increment) {
            if (results[node].done)
                return std::nullopt;
            return item_manual_node(step->node, increment, compiled, results, now_ms);
        }
        if (results[node].done || results[step->node].value > 0)
            return item_manual_node(step->node, increment, compiled, results, now_ms);
        const auto active = std::ranges::find(rule.children, step->node);
        if (active == rule.children.begin())
            return std::nullopt;
        return item_manual_node(*std::prev(active), increment, compiled, results, now_ms);
    }
    if (node >= results.size() || item_show(node, compiled) != Presentation::Show::Cycle)
        return node;
    // Prefer the alternative on screen; otherwise the first one the click can change.
    std::optional<std::uint32_t> target;
    if (const auto step = shown_child(node, compiled, results, now_ms); step && results[step->node].done != increment)
        target = step->node;
    for (const auto child : rule.children) {
        if (!target && child < results.size() && results[child].done != increment)
            target = child;
    }
    if (!target)
        return std::nullopt;
    return item_manual_node(*target, increment, compiled, results, now_ms);
}

ProgressStats summarize_progress(const PublishedState& state, const std::span<const std::uint32_t> goal_nodes) {
    ProgressStats stats{.total = goal_nodes.size()};
    for (const auto node : goal_nodes) {
        if (node < state.snapshot.results.size() && state.snapshot.results[node].done)
            ++stats.completed;
    }
    return stats;
}

}  // namespace beacon
