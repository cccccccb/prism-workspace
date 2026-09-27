#pragma once
#include "prism/contracts/launch.hpp"
#include "prism/contracts/theme.hpp"
#include "prism/launch/package.hpp"
#include "prism/runtime/load_session.hpp"
#include "prism/runtime/session_task_budget.hpp"
#include "prism/runtime/ui_install.hpp"
#include "prism/runtime/ui_load.hpp"
#include "prism/sdk/module_session.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>

namespace prism::sdk {
struct HostConfig {
    std::string socket;
    std::string font_path{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    contracts::RequestId request{1};
    contracts::InstanceId instance{1};
    std::function<void(const contracts::LaunchEvent &)> on_event;
    std::function<std::uint64_t(std::string_view)> launch_app;
    std::function<std::uint64_t()> subscribe_instances;
    std::function<std::uint64_t(std::string_view)> select_theme;
    std::function<std::uint64_t(std::string_view)> select_color_scheme;
    std::optional<std::size_t> gpu_resource_cache_bytes;
    std::optional<contracts::ThemeSnapshot> initial_theme;
    // Optional pure CPU compiler adapter; ownership/thread rules match LoadSession.
    runtime::PrepareFunction prepare_component;
    std::shared_ptr<runtime::SessionTaskBudget> task_budget;
    std::size_t task_workers{2}; // Same pipeline; 1 provides serial preparation.
    runtime::UiInstallLimits install_limits{};
    ModuleSessionLimits module_limits{};
};

// Local observations only; these are not worker/public launch milestones.
struct HostUiState {
    runtime::UiLoadId preview_load;
    runtime::UiLoadId master_load;
    bool preview_submitted{}, preview_presented{};
    bool master_prepared{}, master_installed{}, master_submitted{}, master_presented{};
    bool failed{}, cancelled{};
    std::size_t component_count{}, critical_prepared{}, deferred_prepared{};
    std::size_t deferred_diagnostics{};
    std::size_t deferred_installed{}, deferred_rejected{};
    runtime::UiInstallStats install_stats{};
    bool business_work_pending{}, business_work_completion_ready{};
    bool deferred_started{};
    bool master_images_ready{};
    std::size_t master_image_count{};
    std::uint64_t read_us{}, prepare_us{};
    std::uint64_t master_first_submission{}, master_presented_submission{};
    std::uint64_t submission_count{}, presentation_count{}, last_presented_submission{};
    std::optional<runtime::LoadDiagnostic> master_diagnostic;
};

// Monotonic owner observations, not raw GPU or worker event timestamps.
// Processing time excludes poll waits but includes scheduler descheduling.
struct HostStartupStats {
    std::uint64_t bind_ns{}, master_queued_ns{};
    std::uint64_t preview_submitted_ns{}, preview_presented_ns{};
    std::uint64_t master_prepared_ns{}, master_installed_ns{};
    std::uint64_t master_submitted_ns{}, master_presented_ns{};
    std::uint64_t backend_ready_ns{}, deferred_complete_ns{};
    std::uint64_t frontend_prepare_us{}, preview_prepare_us{};
    std::uint64_t module_load_us{}, module_create_us{};
    std::uint64_t egl_init_us{}, ganesh_init_us{}, first_submit_build_us{};
    std::uint64_t first_render_us{}, first_swap_us{};
    std::uint64_t pump_processing_last_us{}, pump_processing_count{};
    std::uint64_t pump_processing_total_us{}, pump_processing_max_us{};
};

// Constructible in a single-thread seed. PrepareFrontend must run in the final
// worker: it prepares resources and the lazy scheduler. Bind opens a Preview or queues the Master;
// all surface/GPU work remains on the caller's owner thread.
class AppHost {
public:
    explicit AppHost(HostConfig config);
    ~AppHost();
    AppHost(const AppHost &) = delete;
    AppHost &operator=(const AppHost &) = delete;
    bool PrepareFrontend();
    bool Assign(contracts::RequestId request, contracts::InstanceId instance);
    void DeliverLaunchEvent(const contracts::LaunchEvent &event);
    void DeliverInstanceEvent(const contracts::InstanceUpdate &event);
    void DeliverThemeEvent(const contracts::ThemeEvent &event);
    bool ApplyTheme(const contracts::ThemeSnapshot &, std::string *diagnostic = nullptr);
    std::uint64_t ThemeGeneration() const;
    bool Bind(const launch::AppPackage &package);
    // Negative timeout waits for events/deadlines. Descriptors are borrowed;
    // readiness is returned to the caller after Wayland's read lock is released.
    bool Pump(int timeout_ms, std::span<pollfd> wake_fds = {});
    HostUiState GetUiState() const;
    HostStartupStats GetStartupStats() const;
    bool IsCloseRequested() const;
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::sdk
