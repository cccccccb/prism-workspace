#pragma once
#include "prism/contracts/launch.hpp"
#include "prism/contracts/theme.hpp"
#include "prism/launch/package.hpp"
#include "prism/runtime/load_session.hpp"
#include "prism/runtime/ui_load.hpp"
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
};

// Local observations only; these are not worker/public launch milestones.
struct HostUiState {
    runtime::UiLoadId preview_load;
    runtime::UiLoadId master_load;
    bool preview_submitted{}, preview_presented{};
    bool master_prepared{}, master_installed{}, master_submitted{}, master_presented{};
    bool failed{}, cancelled{};
    std::uint64_t read_us{}, prepare_us{};
    std::uint64_t master_first_submission{}, master_presented_submission{};
    std::optional<runtime::LoadDiagnostic> master_diagnostic;
};

// Constructible in a single-thread seed. PrepareFrontend must run in the final
// worker: it creates resource threads. Bind opens a Preview or queues the Master;
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
    bool IsCloseRequested() const;
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::sdk
