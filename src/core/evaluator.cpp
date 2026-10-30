#include <beacon/core/model.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace beacon {
namespace {

ylt::unexpected<Error> internal(std::string message) {
    return ylt::unexpected<Error>{
        {.code = ErrorCode::Internal, .message = std::move(message), .context = "rule graph"}};
}

ylt::expected<void, Error> evaluate_derived(const RuleNode& node, const RuleResults& results, RuleResult& result,
                                            const std::span<const std::uint8_t> evaluated) {
    const auto child = [&](const std::uint32_t child_index) -> const RuleResult* {
        return child_index < results.size() && evaluated[child_index] != 0 ? &results[child_index] : nullptr;
    };
    result = {};
    switch (node.op) {
        case RuleOp::Ref:
            if (const auto* target = child(node.reference_node)) {
                result = *target;
            } else {
                return internal("Ref node has an invalid target index");
            }
            break;
        case RuleOp::All:
        case RuleOp::Any:
        case RuleOp::Count:
            if (node.children.empty())
                return internal("aggregate node has no children");
            for (const auto child_index : node.children) {
                const auto* value = child(child_index);
                if (value == nullptr)
                    return internal("aggregate node has an invalid child index");
                result.value += value->done ? 1 : 0;
            }
            result.target = node.op == RuleOp::All   ? static_cast<std::int64_t>(node.children.size())
                            : node.op == RuleOp::Any ? 1
                                                     : node.target;
            if (result.target < 1 || result.target > static_cast<std::int64_t>(node.children.size()))
                return internal("aggregate node has an invalid target");
            result.done = result.value >= result.target;
            break;
        case RuleOp::Prefix:
            if (node.children.empty())
                return internal("Prefix node has no children");
            result.target = static_cast<std::int64_t>(node.children.size());
            for (const auto child_index : node.children) {
                const auto* value = child(child_index);
                if (value == nullptr)
                    return internal("Prefix node has an invalid child index");
                if (result.active_child == std::numeric_limits<std::uint32_t>::max()) {
                    if (!value->done) {
                        result.active_child = child_index;
                    } else {
                        ++result.value;
                    }
                }
            }
            result.done = result.value == result.target;
            break;
        case RuleOp::Fact:
            return internal("Fact node cannot be recomputed");
        default:
            return internal("rule node has an unknown operation");
    }
    return {};
}

ylt::expected<void, Error> recompute_derived(const CompiledTemplate& compiled, RuleResults& results) {
    const auto& graph = compiled.graph;
    if (results.size() != graph.nodes.size())
        return internal("result count does not match rule graph");
    if (graph.evaluation_order.size() != graph.nodes.size())
        return internal("evaluation order does not contain every rule node");

    std::vector<std::uint8_t> evaluated(graph.nodes.size());
    for (const auto index : graph.evaluation_order) {
        if (index >= graph.nodes.size() || evaluated[index] != 0)
            return internal("evaluation order contains an invalid or duplicate node index");
        const auto& node = graph.nodes[index];
        if (node.op != RuleOp::Fact) {
            if (auto result = evaluate_derived(node, results, results[index], evaluated); !result)
                return result;
        }
        evaluated[index] = 1;
    }
    return {};
}

}  // namespace

ylt::expected<RuleResults, Error> evaluate(const CompiledTemplate& compiled, const Facts& facts) {
    const auto& graph = compiled.graph;
    RuleResults results(graph.nodes.size());
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        const auto& node = graph.nodes[index];
        if (node.op != RuleOp::Fact) {
            continue;
        }
        if (node.target < 1 && !(node.target == 0 && node.fact_key.starts_with("stat/"))) {
            return internal("Fact node has an invalid target");
        }
        auto& result = results[index];
        const auto fact = facts.find(node.fact_key);
        result.value = fact == facts.end() ? 0 : fact->second;
        result.target = node.target;
        result.done = node.target != 0 && result.value >= result.target;
    }
    if (auto derived = recompute_derived(compiled, results); !derived) {
        return ylt::unexpected<Error>{std::move(derived.error())};
    }

    return results;
}

ylt::expected<void, Error> apply_manual_progress(const CompiledTemplate& compiled, RuleResults& results,
                                                 const ManualProgress& manual) {
    if (results.size() != compiled.graph.nodes.size())
        return internal("result count does not match rule graph");
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
    return recompute_derived(compiled, results);
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
