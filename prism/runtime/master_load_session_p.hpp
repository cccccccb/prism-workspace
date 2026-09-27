#pragma once
#include "prism/runtime/master_load_session.hpp"
#include <deque>
#include <map>

namespace prism::runtime {
constexpr std::uint64_t kDslReservation = 32ULL * 1024 * 1024;

struct MasterTaskOutput final : TaskOutput {
    std::shared_ptr<const LoadPlan> plan;
    std::optional<PreparedLayout> layout;
    std::optional<PreparedComponent> prepared;
    std::optional<LoadDiagnostic> diagnostic;
    LoadTimings timings;
    std::size_t source_bytes{};
    std::uint64_t RetainedBytes() const noexcept override;
};

struct MasterReadWork {
    std::filesystem::path root, file;
    ComponentSource source;
    std::shared_ptr<const LoadPlan> plan;
    PrepareFunction prepare;
    bool entry{}, layout{};
    std::shared_ptr<const TaskOutput> operator()(std::stop_token) const;
};

struct MasterComposeWork {
    std::shared_ptr<const LoadPlan> plan;
    PreparedLayout layout;
    std::vector<PreparedUnit> units;
    std::shared_ptr<const TaskOutput> operator()(std::stop_token) const;
};

struct MasterLoadSession::Impl {
    struct UnitState {
        bool submitted{}, complete{};
        std::optional<PreparedComponent> prepared;
    };

    std::shared_ptr<TaskScheduler> scheduler;
    std::shared_ptr<TaskChannel> channel;
    PrepareFunction prepare;
    std::optional<MasterLoadRequest> request;
    std::shared_ptr<const TaskOutput> plan_owner, layout_owner;
    std::shared_ptr<const LoadPlan> plan;
    std::optional<PreparedLayout> layout;
    std::vector<UnitState> units;
    std::optional<MasterLoadCompletion> completion;
    std::vector<LoadDiagnostic> deferred_diagnostics;
    std::deque<MasterRegionCompletion> region_completions;
    MasterLoadStats stats;
    LoadTimings timings;
    bool stopped{}, failed{}, layout_submitted{}, composing{}, delivered{};

    void Drain();
    void Dispatch();
    void Complete(TaskCompletion);
    void Fail(LoadDiagnostic);
    bool DependenciesReady(const LoadUnit &) const;
    void CancelTasks();
};
} // namespace prism::runtime
