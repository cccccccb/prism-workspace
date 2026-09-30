#pragma once
#include "prism/contracts/launch.hpp"
#include "prism/contracts/theme.hpp"
#include "prism/launch/module.hpp"
#include "prism/runtime/property.hpp"
#include "prism/runtime/task_scheduler.hpp"
#include "prism/sdk/layout_control_bridge.hpp"
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>

namespace prism::sdk {
struct ModuleSessionLimits {
    std::size_t outstanding{16};
    std::size_t input_bytes{64 * 1024};
    std::size_t result_bytes{8 * 1024 * 1024};
    std::size_t queued_input_bytes{1024 * 1024}; // May be lowered, never raised above 1 MiB.
    std::size_t completions_per_turn{8};
    std::chrono::microseconds dispatch_budget{2000};
    std::chrono::milliseconds entry_budget{20};
};

struct ModuleWorkState;

// Owner-thread lifecycle adapter. CPU work shares the Host scheduler and never
// receives an instance, graphics object or protocol connection.
class ModuleSession {
public:
    using BindingSink = std::function<bool(std::string_view, runtime::PropertyValue)>;
    using SubscribeSink = std::function<std::uint64_t()>;
    using LaunchSink = std::function<std::uint64_t(std::string_view)>;
    using ColorSchemeSink = std::function<std::uint64_t(std::string_view)>;
    using LayoutSubscribeSink = std::function<std::uint64_t(bool)>;
    using ControlSink = LayoutControlBridge::Send;
    using ThemeSink = std::function<std::uint64_t(std::string_view)>;
    ModuleSession(const std::filesystem::path &module, std::string app_id, std::uint64_t instance,
                  BindingSink bindings, LaunchSink launch = {}, SubscribeSink subscribe = {},
                  ThemeSink themes = {}, ColorSchemeSink schemes = {},
                  std::shared_ptr<runtime::TaskScheduler> scheduler = {},
                  ModuleSessionLimits limits = {}, std::filesystem::path assets_root = {},
                  LayoutSubscribeSink layout_subscribe = {}, ControlSink control = {});
    ~ModuleSession();
    ModuleSession(const ModuleSession &) = delete;
    ModuleSession &operator=(const ModuleSession &) = delete;
    bool Start();
    void Action(std::string_view action);
    void Tick(std::uint64_t now_ns);
    // Negative maximum means infinite; only an explicitly scheduled one-shot
    // tick shortens that wait. Merely defining on_tick does not create a timer.
    int TimeoutMs(std::uint64_t now_ns, int maximum_ms) const;
    void Deliver(const contracts::LaunchEvent &event);
    void Deliver(const contracts::InstanceUpdate &event);
    void Deliver(const contracts::ThemeEvent &event);
    void Gesture(const contracts::GestureEvent &event);
    void Deliver(const contracts::LayoutStateEvent &event);
    void Deliver(const contracts::LayoutControlResult &event);
    void Disconnected();
    int WorkCompletionFd() const noexcept;
    // Checks the cooperative budget between indivisible module callbacks.
    std::size_t DispatchWork();
    bool WorkPending() const noexcept;
    // Permanently rejects Host API calls, cancels and joins this channel only.
    void StopWork() noexcept;
    const std::string &StartDiagnostic() const noexcept;
    std::uint64_t LoadDurationNs() const noexcept;
    std::uint64_t CreateDurationNs() const noexcept;

    bool BackendReady() const
    {
        return OnOwnerThread() && !closed_ && ready_;
    }

private:
    static int32_t SetBinding(void *, PrismStringViewV1, PrismValueV1) noexcept;
    static int32_t Ready(void *) noexcept;
    static uint64_t Launch(void *, PrismStringViewV1) noexcept;
    static uint64_t Subscribe(void *) noexcept;
    static uint64_t SelectTheme(void *, PrismStringViewV1) noexcept;
    static uint64_t SelectColorScheme(void *, PrismStringViewV1) noexcept;
    static int32_t Schedule(void *, uint64_t) noexcept;
    static int32_t SubmitWork(void *, const PrismWorkRequestV1 *) noexcept;
    static int32_t CancelWork(void *, uint64_t) noexcept;
    static uint64_t SubscribeLayout(void *, uint32_t enabled) noexcept;
    static int32_t ControlGesture(void *, const PrismLayoutCommandV1 *) noexcept;
    void DispatchControlResults();
    void DisconnectControl();
    bool OnOwnerThread() const noexcept;
    void DestroyInstance() noexcept;
    const std::thread::id owner_thread_{std::this_thread::get_id()};
    launch::AppModule module_;
    std::string app_id_;
    std::string assets_root_;
    std::uint64_t instance_id_;
    BindingSink bindings_;
    LaunchSink launch_;
    SubscribeSink subscribe_;
    ThemeSink themes_;
    ColorSchemeSink schemes_;
    LayoutSubscribeSink layout_subscribe_;
    LayoutControlBridge controls_;
    std::uint64_t layout_subscription_{};
    bool control_disconnected_{};
    std::map<std::uint64_t, std::string> theme_requests_;
    std::uint64_t subscription_{};

    struct PendingLaunch {
        std::string app_id;
        contracts::InstanceId instance;
        std::uint32_t pid{};
        bool failed{};
    };

    std::map<std::uint64_t, PendingLaunch> launches_;
    PrismHostApiV1 host_;
    void *instance_{};
    std::optional<std::uint64_t> tick_due_;
    bool ready_{};
    bool started_{};
    bool closed_{};
    ModuleSessionLimits limits_;
    std::unique_ptr<ModuleWorkState> work_;
    std::string start_diagnostic_;
    std::uint64_t create_duration_ns_{};
};

std::uint64_t MonotonicNs();
} // namespace prism::sdk
