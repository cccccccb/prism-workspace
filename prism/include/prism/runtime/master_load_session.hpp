#pragma once

#include "prism/runtime/load_plan.hpp"
#include "prism/runtime/load_session.hpp"
#include "prism/runtime/task_scheduler.hpp"
#include <memory>
#include <optional>
#include <span>

namespace prism::runtime {
struct MasterLoadRequest {
    UiLoadId load;
    std::filesystem::path entry;
    std::filesystem::path package_root;
};

struct MasterLoadCompletion {
    UiLoadId load;
    std::shared_ptr<const LoadPlan> plan;
    std::optional<PreparedComponent> prepared;
    std::optional<LoadDiagnostic> diagnostic;
    LoadTimings timings;
};

struct MasterLoadStats {
    std::size_t component_count{}, critical_prepared{}, deferred_prepared{};
    std::size_t source_bytes{};
    bool critical_ready{}, deferred_started{};
};

// Owner-thread graph coordination. Workers only read immutable package files
// and prepare CPU values. Deferred units start after the actual Master feedback;
// their stable-region mounting belongs to the next runtime stage.
class MasterLoadSession {
public:
    MasterLoadSession(std::shared_ptr<TaskScheduler>, PrepareFunction = {});
    ~MasterLoadSession();
    LoadSubmitResult Submit(MasterLoadRequest);
    std::optional<MasterLoadCompletion> TakeCompletion();
    void MasterPresented(UiLoadId);
    void Cancel(UiLoadId);
    void Stop();
    int Fd() const noexcept;
    MasterLoadStats Stats() const;
    std::span<const LoadDiagnostic> DeferredDiagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::runtime
