#include <beacon/app/runtime.hpp>

#include <utility>

namespace beacon {
namespace {

bool same_source(const ActiveMinecraftSource& a, const ActiveMinecraftSource& b) {
    return a.world.path == b.world.path && a.world.identity.storage_key == b.world.identity.storage_key &&
           a.player.uuid == b.player.uuid && a.profile.layout == b.profile.layout &&
           a.world_instance == b.world_instance;
}

struct RebuildCache {
    MinecraftReader reader;
    SourceDiscovery discovery;
    std::optional<ActiveMinecraftSource> previous_source;
    std::optional<std::int64_t> saves_mtime;
    std::chrono::steady_clock::time_point next_discovery{};
};

ylt::expected<std::optional<RebuildResult>, Error> rebuild(const RebuildRequest& request, RebuildCache& cache) {
    auto source = request.source;
    if (!request.game_root.empty()) {
        const auto now = std::chrono::steady_clock::now();
        std::error_code ec;
        const auto saves_time = std::filesystem::last_write_time(request.game_root / "saves", ec);
        const auto mtime =
            ec ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{file_time_milliseconds(saves_time)};
        if (!source || request.discover || now >= cache.next_discovery || ec || cache.saves_mtime != mtime) {
            // ponytail: poll existing worlds once per second; filesystem notifications can reduce
            // switch latency if sub-second detection of changes in inactive worlds becomes necessary.
            cache.next_discovery = now + std::chrono::seconds(1);
            cache.saves_mtime = mtime;
            auto detected =
                cache.discovery.scan(request.game_root, source ? &*source : nullptr, request.compiled.get());
            if (!detected)
                return ylt::unexpected<Error>{std::move(detected.error())};
            source = std::move(*detected);
        }
    }
    if (!source)
        return ylt::unexpected<Error>{
            {ErrorCode::Validation, "Minecraft save directory is not configured", "game_root"}};
    auto instance = directory_identity(source->world.path);
    if (!instance) {
        cache.next_discovery = {};
        return ylt::unexpected<Error>{std::move(instance.error())};
    }
    source->world_instance = std::move(*instance);
    if (!cache.previous_source || !same_source(*source, *cache.previous_source))
        cache.reader.clear();
    cache.previous_source = source;
    auto paths = player_data_paths(source->world, source->player, source->profile);
    if (!paths)
        return ylt::unexpected<Error>{std::move(paths.error())};
    auto read = cache.reader.read(*paths);
    if (!read)
        return ylt::unexpected<Error>{std::move(read.error())};
    if (!*read)
        return std::optional<RebuildResult>{};
    auto& facts = **read;
    source->profile.data_version = facts.data_version;
    if (!supports_template(source->profile, *request.compiled))
        return ylt::unexpected<Error>{{ErrorCode::UnsupportedVersion,
                                       "detected Minecraft version is incompatible with the template", "modern-json"}};
    auto results = evaluate(*request.compiled, facts.facts);
    if (!results)
        return ylt::unexpected<Error>{std::move(results.error())};
    const auto clock = facts.facts.find("clock/play_ticks");
    if (clock == facts.facts.end())
        return ylt::unexpected<Error>{{ErrorCode::Internal, "Minecraft facts have no play time", "worker"}};
    return std::optional<RebuildResult>{{std::move(*source), std::move(*results), clock->second}};
}

}  // namespace

RebuildWorker::RebuildWorker()
    : thread_([this](std::stop_token stop) {
          run(stop);
      }) {}

RebuildWorker::~RebuildWorker() {
    thread_.request_stop();
    wake_.notify_all();
}

void RebuildWorker::submit(RebuildRequest request) {
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(request);
    }
    wake_.notify_one();
}

std::optional<WorkerMessage> RebuildWorker::wait_for_result(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!result_ready_.wait_for(lock, timeout, [&] {
            return result_.has_value();
        })) {
        return std::nullopt;
    }
    return std::exchange(result_, std::nullopt);
}

void RebuildWorker::run(std::stop_token stop) {
    RebuildCache cache;
    std::optional<std::uint64_t> generation;
    while (!stop.stop_requested()) {
        std::optional<RebuildRequest> request;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [&] {
                return pending_.has_value();
            });
            if (stop.stop_requested()) {
                return;
            }
            request = std::exchange(pending_, std::nullopt);
        }
        if (generation != request->generation) {
            cache = {};
            generation = request->generation;
        }
        auto rebuilt = rebuild(*request, cache);
        if (!rebuilt)
            cache.reader.clear();  // Retry incomplete writes even when their metadata stays unchanged.
        WorkerMessage result{request->generation, std::move(rebuilt), cache.discovery.warnings};
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(result);
        }
        result_ready_.notify_one();
    }
}

void Runtime::begin_run(Snapshot empty) {
    manual_ = {};
    base_results_.clear();
    ++generation_;
    ++run_epoch_;
    in_flight_ = false;
    empty.run_epoch = run_epoch_;
    replace_state({}, template_, std::move(empty), DataStatus::Unavailable);
}

void Runtime::replace_state(RunIdentity run, std::shared_ptr<const CompiledTemplate> compiled, Snapshot snapshot,
                            const DataStatus status, const std::int64_t last_read, std::vector<Error> warnings) {
    state_ = std::make_shared<const PublishedState>(PublishedState{.run = std::move(run),
                                                                   .compiled = std::move(compiled),
                                                                   .snapshot = std::move(snapshot),
                                                                   .localization = localization_,
                                                                   .layout = layout_,
                                                                   .data_status = status,
                                                                   .last_successful_read_at = last_read,
                                                                   .warnings = std::move(warnings)});
}

ylt::expected<void, Error> Runtime::select(RunSelection selection) {
    if (!selection.compiled || !selection.localization || !selection.layout ||
        !valid_path_component(selection.world.identity.storage_key) || !valid_player_id(selection.player.uuid)) {
        return ylt::unexpected<Error>{
            {.code = ErrorCode::Validation, .message = "invalid run selection", .context = "selection"}};
    }
    auto paths = player_data_paths(selection.world, selection.player, selection.profile);
    if (!paths) {
        return ylt::unexpected<Error>{std::move(paths.error())};
    }

    auto prepared =
        prepare_configuration(selection.game_root, selection.compiled, selection.localization, selection.layout);
    if (!prepared)
        return ylt::unexpected<Error>{std::move(prepared.error())};
    game_root_ = std::move(selection.game_root);
    template_ = std::move(selection.compiled);
    localization_ = std::move(selection.localization);
    layout_ = std::move(selection.layout);
    source_ = ActiveMinecraftSource{
        std::move(selection.world), std::move(selection.player), std::move(selection.profile), {}};
    begin_run(std::move(prepared->empty_));
    submit();
    return {};
}

ylt::expected<void, Error> Runtime::configure(std::filesystem::path game_root,
                                              std::shared_ptr<const CompiledTemplate> compiled,
                                              std::shared_ptr<const Localization> localization,
                                              std::shared_ptr<const Layout> layout) {
    auto prepared =
        prepare_configuration(std::move(game_root), std::move(compiled), std::move(localization), std::move(layout));
    if (!prepared)
        return ylt::unexpected<Error>{std::move(prepared.error())};
    commit_configuration(std::move(*prepared));
    return {};
}

ylt::expected<RuntimeConfiguration, Error>
Runtime::prepare_configuration(std::filesystem::path game_root, std::shared_ptr<const CompiledTemplate> compiled,
                               std::shared_ptr<const Localization> localization, std::shared_ptr<const Layout> layout) {
    if (!compiled || !localization || !layout)
        return ylt::unexpected<Error>{
            {ErrorCode::Validation, "template resources are not configured", "configuration"}};
    auto results = evaluate(*compiled, {});
    if (!results)
        return ylt::unexpected<Error>{std::move(results.error())};
    auto empty = publish(*compiled, std::move(*results), {}, nullptr);
    if (!empty)
        return ylt::unexpected<Error>{std::move(empty.error())};
    RuntimeConfiguration configuration;
    configuration.game_root_ = std::move(game_root);
    configuration.compiled_ = std::move(compiled);
    configuration.localization_ = std::move(localization);
    configuration.layout_ = std::move(layout);
    configuration.empty_ = std::move(*empty);
    return configuration;
}

void Runtime::commit_configuration(RuntimeConfiguration configuration) {
    const bool preserve = state_ && template_ && game_root_ == configuration.game_root_ &&
                          template_->same_rules(*configuration.compiled_);
    game_root_ = std::move(configuration.game_root_);
    template_ = std::move(configuration.compiled_);
    localization_ = std::move(configuration.localization_);
    layout_ = std::move(configuration.layout_);
    if (preserve) {
        auto state = std::make_shared<PublishedState>(*state_);
        state->compiled = template_;
        state->localization = localization_;
        state->layout = layout_;
        state_ = std::move(state);
        return;
    }
    source_.reset();
    begin_run(std::move(configuration.empty_));
    if (game_root_.empty()) {
        publish_error({ErrorCode::Validation, "Minecraft save directory is not configured", "game_root"});
    } else {
        submit();
    }
}

bool Runtime::poll_files(const bool discover) {
    if (!template_ || (game_root_.empty() && !source_) || in_flight_)
        return false;
    submit(discover);
    return true;
}

void Runtime::submit(const bool discover) {
    worker_.submit({generation_, game_root_, source_, template_, discover});
    in_flight_ = true;
}

ylt::expected<bool, Error> Runtime::process_next(std::chrono::milliseconds timeout) {
    auto message = worker_.wait_for_result(timeout);
    if (!message) {
        return false;
    }
    return apply_worker_message(std::move(*message));
}

ylt::expected<void, Error> Runtime::manual_operation(const std::uint32_t node, const bool increment) {
    if (!state_ || !state_->compiled || state_->data_status != DataStatus::Ready)
        return ylt::unexpected<Error>{
            {.code = ErrorCode::Validation, .message = "manual progress is unavailable", .context = "runtime"}};
    auto manual = manual_;
    auto results = base_results_;
    auto applied = apply_manual_operation(*state_->compiled, results, manual, node, increment);
    if (!applied)
        return ylt::unexpected<Error>{std::move(applied.error())};
    auto snapshot = publish(*state_->compiled, std::move(results),
                            {.run_epoch = state_->snapshot.run_epoch, .play_ticks = state_->snapshot.play_ticks},
                            &state_->snapshot);
    if (!snapshot)
        return ylt::unexpected<Error>{std::move(snapshot.error())};
    manual_ = std::move(manual);
    replace_state(state_->run, state_->compiled, std::move(*snapshot), DataStatus::Ready,
                  state_->last_successful_read_at, state_->warnings);
    return {};
}

void Runtime::publish_error(Error error, std::vector<Error> warnings) {
    if (!state_ || (state_->error == error && state_->data_status != DataStatus::Ready && state_->warnings == warnings))
        return;
    auto state = std::make_shared<PublishedState>(*state_);
    state->data_status = state->has_data() ? DataStatus::Stale : DataStatus::Unavailable;
    state->error = std::move(error);
    state->warnings = std::move(warnings);
    state_ = std::move(state);
}

ylt::expected<void, Error> Runtime::restart_run() {
    if (!state_ || !template_)
        return ylt::unexpected<Error>{{ErrorCode::Validation, "no run to restart", "runtime"}};
    auto prepared = prepare_configuration(game_root_, template_, localization_, layout_);
    if (!prepared)
        return ylt::unexpected<Error>{std::move(prepared.error())};
    begin_run(std::move(prepared->empty_));
    submit(true);
    return {};
}

ylt::expected<bool, Error> Runtime::apply_worker_message(WorkerMessage message) {
    if (message.generation != generation_)
        return false;
    in_flight_ = false;
    if (!message.result) {
        auto error = std::move(message.result.error());
        publish_error(error, std::move(message.warnings));
        return ylt::unexpected<Error>{std::move(error)};
    }
    if (!*message.result) {
        const bool diagnostics_changed = state_ && state_->warnings != message.warnings;
        if (diagnostics_changed) {
            auto state = std::make_shared<PublishedState>(*state_);
            state->warnings = std::move(message.warnings);
            state_ = std::move(state);
        }
        return diagnostics_changed;
    }
    auto result = std::move(**message.result);
    const bool new_run = state_ && state_->has_data() && source_ &&
                         (!same_source(*source_, result.source) || result.play_ticks < state_->snapshot.play_ticks);
    if (new_run) {
        manual_ = {};
        base_results_.clear();
        ++run_epoch_;
    }
    const Snapshot* previous = !new_run && state_ && state_->has_data() ? &state_->snapshot : nullptr;
    source_ = std::move(result.source);
    auto results = std::move(result.rule_results);
    base_results_ = results;
    if (!manual_.completed.empty() || !manual_.stat_deltas.empty()) {
        if (auto applied = apply_manual_progress(*template_, results, manual_); !applied) {
            publish_error(applied.error(), std::move(message.warnings));
            return ylt::unexpected<Error>{std::move(applied.error())};
        }
    }
    auto snapshot = publish(*template_, std::move(results), {run_epoch_, result.play_ticks}, previous);
    if (!snapshot) {
        publish_error(snapshot.error(), std::move(message.warnings));
        return ylt::unexpected<Error>{std::move(snapshot.error())};
    }
    const auto last_read = snapshot->updated_at;
    replace_state({source_->world.identity, source_->player}, template_, std::move(*snapshot), DataStatus::Ready,
                  last_read, std::move(message.warnings));
    return true;
}

}  // namespace beacon
