#include <beacon/core/model.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace beacon {
namespace {

ylt::unexpected<Error> internal(std::string message) {
    return ylt::unexpected<Error>{
        {.code = ErrorCode::Internal, .message = std::move(message), .context = "rule graph"}};
}

// The compiler orders every dependency before the nodes that use it.
void recompute_derived(const RuleGraph& graph, RuleResults& results) {
    for (const auto index : graph.evaluation_order) {
        const auto& node = graph.nodes[index];
        auto& result = results[index];
        switch (node.op) {
            case RuleOp::Ref:
                result = results[node.reference_node];
                break;
            case RuleOp::All:
            case RuleOp::Any:
            case RuleOp::Count:
                result = {};
                for (const auto child : node.children)
                    result.value += results[child].done ? 1 : 0;
                result.target = node.op == RuleOp::All   ? static_cast<std::int64_t>(node.children.size())
                                : node.op == RuleOp::Any ? 1
                                                         : node.target;
                result.done = result.value >= result.target;
                break;
            case RuleOp::Prefix:
                result = {};
                result.target = static_cast<std::int64_t>(node.children.size());
                for (const auto child : node.children) {
                    if (result.active_child != std::numeric_limits<std::uint32_t>::max())
                        break;
                    if (results[child].done)
                        ++result.value;
                    else
                        result.active_child = child;
                }
                result.done = result.value == result.target;
                break;
            default:
                break;
        }
    }
}

}  // namespace

RuleResults evaluate(const CompiledTemplate& compiled, const Facts& facts) {
    const auto& graph = compiled.graph;
    RuleResults results(graph.nodes.size());
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        const auto& node = graph.nodes[index];
        if (node.op != RuleOp::Fact) {
            continue;
        }
        auto& result = results[index];
        const auto fact = facts.find(node.fact_key);
        result.value = fact == facts.end() ? 0 : fact->second;
        result.target = node.target;
        result.done = node.target != 0 && result.value >= result.target;
    }
    recompute_derived(graph, results);
    return results;
}

void apply_manual_progress(const CompiledTemplate& compiled, RuleResults& results, const ManualProgress& manual) {
    for (const auto node_index : manual.completed) {
        auto& result = results[node_index];
        result.value = std::max(result.value, result.target);
        result.done = true;
    }
    for (const auto& [node_index, delta] : manual.stat_deltas) {
        auto& result = results[node_index];
        result.value = std::max<std::int64_t>(0, result.value + delta);
        result.done = result.target != 0 && result.value >= result.target;
    }
    recompute_derived(compiled.graph, results);
}

ylt::expected<void, Error> apply_manual_operation(const CompiledTemplate& compiled, RuleResults& results,
                                                  ManualProgress& manual, const std::uint32_t node,
                                                  const bool increment) {
    if (node >= compiled.graph.nodes.size() || compiled.graph.nodes[node].op != RuleOp::Fact)
        return internal("manual operation targets a non-Fact node");
    if (compiled.graph.nodes[node].fact_key.starts_with("stat/")) {
        auto& delta = manual.stat_deltas[node];
        delta += increment ? 1 : -1;
        if (delta == 0)
            manual.stat_deltas.erase(node);
    } else if (increment) {
        manual.completed.insert(node);
    } else {
        manual.completed.erase(node);
    }
    apply_manual_progress(compiled, results, manual);
    return {};
}

Snapshot publish(const CompiledTemplate& compiled, RuleResults results, const PublishContext& context,
                 const Snapshot* previous) {
    Snapshot snapshot;
    snapshot.revision = previous == nullptr ? 1 : previous->revision + 1;
    snapshot.updated_at =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    snapshot.run_epoch = context.run_epoch;
    snapshot.results = std::move(results);
    snapshot.play_ticks = context.play_ticks;
    if (snapshot.results[compiled.completion_node].done) {
        snapshot.completion_play_ticks = previous != nullptr && previous->run_epoch == context.run_epoch
                                             ? previous->completion_play_ticks.value_or(context.play_ticks)
                                             : context.play_ticks;
    }
    return snapshot;
}

}  // namespace beacon
