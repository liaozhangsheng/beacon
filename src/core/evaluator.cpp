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

// The compiler only builds valid graphs, but a CompiledTemplate can be changed afterwards. Everything evaluation relies
// on is checked here, once, before any result is computed.
ylt::expected<void, Error> check_graph(const RuleGraph& graph, const RuleResults& results) {
    const auto size = graph.nodes.size();
    if (results.size() != size)
        return internal("result count does not match rule graph");
    if (graph.evaluation_order.size() != size)
        return internal("evaluation order does not contain every rule node");
    std::vector<std::uint8_t> evaluated(size);
    const auto ready = [&](const std::uint32_t index) {
        return index < size && evaluated[index] != 0;
    };
    for (const auto index : graph.evaluation_order) {
        if (index >= size || evaluated[index] != 0)
            return internal("evaluation order contains an invalid or duplicate node index");
        const auto& node = graph.nodes[index];
        switch (node.op) {
            case RuleOp::Fact:
                if (node.target < 1 && !(node.target == 0 && node.fact_key.starts_with("stat/")))
                    return internal("Fact node has an invalid target");
                break;
            case RuleOp::Ref:
                if (!ready(node.reference_node))
                    return internal("Ref node has an invalid target index");
                break;
            case RuleOp::All:
            case RuleOp::Any:
            case RuleOp::Count:
            case RuleOp::Prefix:
                if (node.children.empty())
                    return internal("aggregate node has no children");
                if (!std::ranges::all_of(node.children, ready))
                    return internal("aggregate node has an invalid child index");
                if (node.op == RuleOp::Count &&
                    (node.target < 1 || node.target > static_cast<std::int64_t>(node.children.size())))
                    return internal("aggregate node has an invalid target");
                break;
            default:
                return internal("rule node has an unknown operation");
        }
        evaluated[index] = 1;
    }
    return {};
}

// The graph has passed check_graph, so every dependency comes earlier in the evaluation order.
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

ylt::expected<RuleResults, Error> evaluate(const CompiledTemplate& compiled, const Facts& facts) {
    const auto& graph = compiled.graph;
    RuleResults results(graph.nodes.size());
    if (auto checked = check_graph(graph, results); !checked)
        return ylt::unexpected<Error>{std::move(checked.error())};
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

ylt::expected<void, Error> apply_manual_progress(const CompiledTemplate& compiled, RuleResults& results,
                                                 const ManualProgress& manual) {
    if (auto checked = check_graph(compiled.graph, results); !checked)
        return checked;
    for (const auto node_index : manual.completed) {
        if (node_index >= compiled.graph.nodes.size() || compiled.graph.nodes[node_index].op != RuleOp::Fact)
            return internal("manual completion targets a non-Fact node");
        auto& result = results[node_index];
        result.value = std::max(result.value, result.target);
        result.done = true;
    }
    for (const auto& [node_index, delta] : manual.stat_deltas) {
        if (node_index >= compiled.graph.nodes.size() || compiled.graph.nodes[node_index].op != RuleOp::Fact ||
            !compiled.graph.nodes[node_index].fact_key.starts_with("stat/"))
            return internal("manual statistic targets an invalid node");
        auto& result = results[node_index];
        if (delta > 0 && result.value > std::numeric_limits<std::int64_t>::max() - delta)
            result.value = std::numeric_limits<std::int64_t>::max();
        else if (delta < 0 && (delta == std::numeric_limits<std::int64_t>::min() || result.value < -delta))
            result.value = 0;
        else
            result.value = std::max<std::int64_t>(0, result.value + delta);
        result.done = result.target != 0 && result.value >= result.target;
    }
    recompute_derived(compiled.graph, results);
    return {};
}

ylt::expected<void, Error> apply_manual_operation(const CompiledTemplate& compiled, RuleResults& results,
                                                  ManualProgress& manual, const std::uint32_t node,
                                                  const bool increment) {
    if (node >= compiled.graph.nodes.size() || compiled.graph.nodes[node].op != RuleOp::Fact)
        return internal("manual operation targets a non-Fact node");
    if (compiled.graph.nodes[node].fact_key.starts_with("stat/")) {
        auto& delta = manual.stat_deltas[node];
        if ((increment && delta == std::numeric_limits<std::int64_t>::max()) ||
            (!increment && delta == std::numeric_limits<std::int64_t>::min()))
            return internal("manual statistic delta overflow");
        delta += increment ? 1 : -1;
        if (delta == 0)
            manual.stat_deltas.erase(node);
    } else if (increment) {
        manual.completed.insert(node);
    } else {
        manual.completed.erase(node);
    }
    return apply_manual_progress(compiled, results, manual);
}

ylt::expected<Snapshot, Error> publish(const CompiledTemplate& compiled, RuleResults results,
                                       const PublishContext& context, const Snapshot* previous) {
    if (results.size() != compiled.graph.nodes.size()) {
        return internal("result count does not match rule graph");
    }
    if (compiled.completion_node >= results.size()) {
        return internal("completion rule index is invalid");
    }
    if (context.play_ticks < 0) {
        return ylt::unexpected<Error>{
            {.code = ErrorCode::Internal, .message = "play ticks must be non-negative", .context = "publish context"}};
    }
    if (previous != nullptr && previous->revision == std::numeric_limits<std::uint64_t>::max()) {
        return internal("snapshot revision overflow");
    }

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
