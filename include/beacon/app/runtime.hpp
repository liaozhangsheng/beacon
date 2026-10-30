#pragma once

#include <beacon/minecraft/adapter.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace beacon {

struct Layout;
struct Localization;

struct RunSelection {
    WorldLocation world;
    PlayerIdentity player;
    std::shared_ptr<const CompiledTemplate> compiled;
    VersionProfile profile;
    std::shared_ptr<const Localization> localization;
    std::shared_ptr<const Layout> layout;
    std::filesystem::path game_root;
};

struct RebuildRequest {
    std::uint64_t generation = 0;
    std::filesystem::path game_root;
    std::optional<ActiveMinecraftSource> source;
    std::shared_ptr<const CompiledTemplate> compiled;
    bool discover = false;
};

struct RebuildResult {
    ActiveMinecraftSource source;
    RuleResults rule_results;
    std::int64_t play_ticks = 0;
};

struct WorkerMessage {
    std::uint64_t generation = 0;
    // An empty successful result means the source files have not changed.
    ylt::expected<std::optional<RebuildResult>, Error> result;
    std::vector<Error> warnings;
};

enum class DataStatus { Ready, Stale, Unavailable };

struct PublishedState {
    RunIdentity run;
    std::shared_ptr<const CompiledTemplate> compiled;
    Snapshot snapshot;
    std::shared_ptr<const Localization> localization;
    std::shared_ptr<const Layout> layout;
    DataStatus data_status = DataStatus::Ready;
    std::int64_t last_successful_read_at = 0;
    std::optional<Error> error;
    std::vector<Error> warnings;

    [[nodiscard]] bool has_data() const {
        return data_status != DataStatus::Unavailable;
    }
};

class RebuildWorker {
public:
    RebuildWorker();
    ~RebuildWorker();
    RebuildWorker(const RebuildWorker&) = delete;
    RebuildWorker& operator=(const RebuildWorker&) = delete;

    void submit(RebuildRequest request);
    std::optional<WorkerMessage> wait_for_result(std::chrono::milliseconds timeout);

private:
    void run(std::stop_token stop);

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable result_ready_;
    std::optional<RebuildRequest> pending_;
    std::optional<WorkerMessage> result_;
    std::jthread thread_;
};

// Prepared without changing the live run. Commit only after settings are saved.
class RuntimeConfiguration {
    friend class Runtime;
    RuntimeConfiguration() = default;
    std::filesystem::path game_root_;
    std::shared_ptr<const CompiledTemplate> compiled_;
    std::shared_ptr<const Localization> localization_;
    std::shared_ptr<const Layout> layout_;
    Snapshot empty_;
};

class Runtime {
public:
    Runtime() = default;

    ylt::expected<void, Error> select(RunSelection selection);
    ylt::expected<void, Error> configure(std::filesystem::path game_root,
                                         std::shared_ptr<const CompiledTemplate> compiled,
                                         std::shared_ptr<const Localization> localization,
                                         std::shared_ptr<const Layout> layout);
    static ylt::expected<RuntimeConfiguration, Error>
    prepare_configuration(std::filesystem::path game_root, std::shared_ptr<const CompiledTemplate> compiled,
                          std::shared_ptr<const Localization> localization, std::shared_ptr<const Layout> layout);
    void commit_configuration(RuntimeConfiguration configuration);
    // Queues a background scan; true means queued, not necessarily changed.
    bool poll_files(bool discover = false);
    ylt::expected<void, Error> restart_run();
    ylt::expected<bool, Error> process_next(std::chrono::milliseconds timeout);
    ylt::expected<void, Error> manual_operation(std::uint32_t node, bool increment);

    [[nodiscard]] std::shared_ptr<const PublishedState> state() const {
        return state_;
    }

private:
    void begin_run(Snapshot empty);
    void replace_state(RunIdentity run, std::shared_ptr<const CompiledTemplate> compiled, Snapshot snapshot,
                       DataStatus status, std::int64_t last_read = 0, std::vector<Error> warnings = {});
    void publish_error(Error error, std::vector<Error> warnings = {});
    void submit(bool discover = false);
    ylt::expected<bool, Error> apply_worker_message(WorkerMessage message);

    RebuildWorker worker_;
    std::optional<ActiveMinecraftSource> source_;
    bool in_flight_ = false;
    std::filesystem::path game_root_;
    std::shared_ptr<const CompiledTemplate> template_;
    std::shared_ptr<const Localization> localization_;
    std::shared_ptr<const Layout> layout_;
    std::shared_ptr<const PublishedState> state_;
    std::uint64_t generation_ = 0;
    std::uint64_t run_epoch_ = 0;
    ManualProgress manual_;
    RuleResults base_results_;
};

}  // namespace beacon
