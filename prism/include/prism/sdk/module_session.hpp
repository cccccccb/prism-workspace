#pragma once
#include "prism/contracts/launch.hpp"
#include "prism/contracts/theme.hpp"
#include "prism/launch/module.hpp"
#include "prism/runtime/property.hpp"
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace prism::sdk {
// Business lifecycle adapter. No graphics, protocol connection or background thread.
class ModuleSession {
public:
    using BindingSink = std::function<bool(std::string_view, runtime::PropertyValue)>;
    using SubscribeSink = std::function<std::uint64_t()>;
    using LaunchSink = std::function<std::uint64_t(std::string_view)>;
    using ColorSchemeSink = std::function<std::uint64_t(std::string_view)>;
    using ThemeSink = std::function<std::uint64_t(std::string_view)>;
    ModuleSession(const std::filesystem::path &module, std::string app_id, std::uint64_t instance,
                  BindingSink bindings, LaunchSink launch = {}, SubscribeSink subscribe = {},
                  ThemeSink themes = {}, ColorSchemeSink schemes = {});
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
    void Disconnected();

    bool BackendReady() const
    {
        return ready_;
    }

private:
    static int32_t SetBinding(void *, PrismStringViewV1, PrismValueV1) noexcept;
    static int32_t Ready(void *) noexcept;
    static uint64_t Launch(void *, PrismStringViewV1) noexcept;
    static uint64_t Subscribe(void *) noexcept;
    static uint64_t SelectTheme(void *, PrismStringViewV1) noexcept;
    static uint64_t SelectColorScheme(void *, PrismStringViewV1) noexcept;
    static int32_t Schedule(void *, uint64_t) noexcept;
    launch::AppModule module_;
    std::string app_id_;
    std::uint64_t instance_id_;
    BindingSink bindings_;
    LaunchSink launch_;
    SubscribeSink subscribe_;
    ThemeSink themes_;
    ColorSchemeSink schemes_;
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
};

std::uint64_t MonotonicNs();
} // namespace prism::sdk
