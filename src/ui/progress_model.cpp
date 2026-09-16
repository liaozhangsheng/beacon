#include <beacon/ui/progress.hpp>

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

    const auto collections = [&](const LayoutGroup& group, const std::vector<std::uint32_t>& nodes) {
        std::vector<Collection> resolved;
        if (group.source != LayoutSource::Children) {
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

ProgressStats summarize_progress(const PublishedState& state, const std::span<const std::uint32_t> goal_nodes) {
    ProgressStats stats{.total = goal_nodes.size()};
    for (const auto node : goal_nodes) {
        if (node < state.snapshot.results.size() && state.snapshot.results[node].done)
            ++stats.completed;
    }
    return stats;
}

}  // namespace beacon
