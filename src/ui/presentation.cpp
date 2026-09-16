#include <beacon/ui/display.hpp>

#include <algorithm>
#include <unordered_set>

namespace beacon {

std::vector<std::uint32_t> resolve_layout_nodes(const LayoutGroup& group, const CompiledTemplate& compiled,
                                                const std::size_t result_count) {
    std::vector<std::uint32_t> nodes;
    std::unordered_set<std::uint32_t> seen;
    const auto add = [&](const std::uint32_t index) {
        if (index < result_count && seen.insert(index).second) {
            nodes.push_back(index);
        }
    };
    if (group.source == LayoutSource::Nodes) {
        for (const auto& id : group.nodes)
            add(compiled.graph.index_by_id.at(id));
    } else if (group.source == LayoutSource::Children) {
        for (const auto& id : group.nodes) {
            for (const auto child : compiled.graph.nodes[compiled.graph.index_by_id.at(id)].children)
                add(child);
        }
    } else {
        for (std::uint32_t index = 0; index < compiled.graph.nodes.size(); ++index) {
            for (const auto& prefix : group.nodes) {
                if (compiled.graph.nodes[index].id.starts_with(prefix)) {
                    add(index);
                    break;
                }
            }
        }
    }

    std::unordered_set<std::uint32_t> excluded;
    for (const auto& id : group.exclude)
        excluded.insert(compiled.graph.index_by_id.at(id));
    std::erase_if(nodes, [&](const auto index) {
        return excluded.contains(index);
    });
    return nodes;
}

std::string display_label(const std::uint32_t node, const CompiledTemplate& compiled,
                          const Localization& localization) {
    if (const auto& presentation = compiled.presentation_by_node[node]) {
        return std::string(localization.text(presentation->localization_key));
    }
    auto label = compiled.graph.nodes[node].fact_key;
    if (const auto slash = label.rfind('/'); slash != std::string::npos)
        label.erase(0, slash + 1);
    if (label.starts_with("minecraft:"))
        label.erase(0, 10);
    std::replace(label.begin(), label.end(), '_', ' ');
    return label;
}

}  // namespace beacon
