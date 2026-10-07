#pragma once
#include "prism/contracts/launch.hpp"
#include "prism/contracts/owner_feedback.hpp"
#include "prism/contracts/owner_task.hpp"
#include "prism/contracts/theme.hpp"
#include "prism/launch/module.hpp"
#include "prism/runtime/control_value.hpp"
#include "prism/runtime/property.hpp"
#include "prism/runtime/task_scheduler.hpp"
#include "prism/sdk/layout_control_bridge.hpp"
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
struct ModuleTaskState;
struct ModuleFeedbackState;

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
    // Sinks receive owning typed data. TaskSink stages without delivering a
    // terminal callback; Host binds correlation to its actual frontend owner.
    using TaskSink = std::function<bool(const contracts::OwnerTaskRequest &)>;
    using TaskCancelSink = std::function<bool(std::uint64_t)>;
    using TaskCapabilitySink = std::function<std::uint32_t()>;
    using FeedbackSink = std::function<bool(const contracts::OwnerFeedbackRequest &)>;
    using FeedbackDismissSink = std::function<bool(std::uint64_t)>;
    using FeedbackCapabilitySink = std::function<std::uint32_t()>;
    ModuleSession(const std::filesystem::path &module, std::string app_id, std::uint64_t instance,
                  BindingSink bindings, LaunchSink launch = {}, SubscribeSink subscribe = {},
                  ThemeSink themes = {}, ColorSchemeSink schemes = {},
                  std::shared_ptr<runtime::TaskScheduler> scheduler = {},
                  ModuleSessionLimits limits = {}, std::filesystem::path assets_root = {},
                  LayoutSubscribeSink layout_subscribe = {}, ControlSink control = {},
                  TaskSink tasks = {}, TaskCancelSink cancel_tasks = {},
                  TaskCapabilitySink task_capabilities = {}, FeedbackSink feedback = {},
                  FeedbackDismissSink dismiss_feedback = {},
                  FeedbackCapabilitySink feedback_capabilities = {});
    ~ModuleSession();
    ModuleSession(const ModuleSession &) = delete;
    ModuleSession &operator=(const ModuleSession &) = delete;
    bool Start();
    void Action(std::string_view action);
    bool RequestClose();
    // Deferred completion only. Never consume inside a module close callback.
    std::optional<bool> ConsumeCloseDecision() noexcept;
    void TextEdit(std::string_view action, std::string_view text);
    void ControlValue(const runtime::ControlEdit &edit);
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
    bool SupportsOwnerTasks() const noexcept;
    // Rejects invalid, duplicate, stale results and delivery during submission.
    // Removes the matching pending request before invoking the business module.
    bool DeliverTaskResult(const contracts::OwnerTaskResult &result);
    bool SupportsFeedback() const noexcept;
    bool DeliverFeedbackAction(const contracts::OwnerFeedbackAction &action);
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
    static uint32_t TaskCapabilities(void *) noexcept;
    static uint64_t RequestTask(void *, const PrismTaskRequestV1 *) noexcept;
    static int32_t CancelTask(void *, uint64_t request_id) noexcept;
    static int32_t CompleteClose(void *, uint64_t request_id, uint32_t decision) noexcept;
    static uint32_t FeedbackCapabilities(void *) noexcept;
    static uint64_t ShowFeedback(void *, const PrismFeedbackRequestV1 *) noexcept;
    static int32_t DismissFeedback(void *, uint64_t request_id) noexcept;
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
    std::uint64_t next_close_id_{1};
    std::optional<std::uint64_t> pending_close_;
    std::optional<bool> close_decision_;
    bool close_callback_active_{};
    bool close_accepted_{};
    ModuleSessionLimits limits_;
    std::unique_ptr<ModuleWorkState> work_;
    std::unique_ptr<ModuleTaskState> tasks_;
    std::unique_ptr<ModuleFeedbackState> feedback_;
    std::string start_diagnostic_;
    std::uint64_t create_duration_ns_{};
};

std::uint64_t MonotonicNs();
} // namespace prism::sdk
