#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <map>
#include <string>
using namespace prism;

int main(int argc, char **argv)
{
    assert(argc == 5);
    for (int n = 1; n < 5; ++n) {
        std::map<std::string, runtime::PropertyValue> bindings;
        std::string launched;
        std::string selected_theme, selected_scheme;
        std::uint64_t theme_request{};
        sdk::ModuleSession module(
            argv[n], "test", 22,
            [&](auto key, auto value) {
                bindings[std::string(key)] = std::move(value);
                return true;
            },
            [&](auto app) {
                launched = app;
                return 6;
            },
            [] { return 5; },
            [&](auto id) {
                selected_theme = id;
                return ++theme_request;
            },
            [&](auto scheme) {
                selected_scheme = scheme;
                return ++theme_request;
            });
        assert(module.Start() && module.BackendReady());
        if (n == 3) {
            auto visible = [&](const char *name) {
                return std::get<bool>(bindings.at(name));
            };
            auto icon = [&](const char *name) {
                return std::get<std::string>(bindings.at(name));
            };
            assert(!visible("music_visible") && !visible("settings_visible") &&
                   !visible("running_visible"));
            assert(!visible("music_feedback_visible") && !visible("settings_feedback_visible"));
            module.Action("app:center");
            assert(launched.empty()); // Launcher UI deferred.
            module.Deliver(contracts::InstanceUpdate{
                {5}, {8}, 42, contracts::InstanceChange::Running, "demo_player"});
            module.Deliver(contracts::InstanceUpdate{
                {5}, {8}, 42, contracts::InstanceChange::Running, "demo_player"});
            assert(visible("music_visible") && visible("running_visible"));
            module.Action("app:launch:music");
            assert(launched == "demo_player");
            assert(visible("music_feedback_visible") && icon("music_feedback_icon") == "refresh");
            module.Deliver(
                contracts::LaunchEvent{{6}, {8}, 42, contracts::LaunchMilestone::Accepted});
            assert(icon("music_feedback_icon") == "refresh");
            module.Deliver(
                contracts::LaunchEvent{{6}, {8}, 42, contracts::LaunchMilestone::Activated});
            assert(icon("music_feedback_icon") == "check");
            module.Action("app:launch:settings");
            assert(launched == "demo_settings");
            module.Deliver(
                contracts::LaunchEvent{{6}, {9}, 43, contracts::LaunchMilestone::BackendReady});
            assert(icon("settings_feedback_icon") == "refresh");
            // Launch completion must not invent a mapped instance.
            assert(!visible("settings_visible"));
            module.Deliver(
                contracts::LaunchEvent{{6}, {9}, 43, contracts::LaunchMilestone::FirstPresented});
            assert(icon("settings_feedback_icon") == "check");
            module.Deliver(contracts::InstanceUpdate{
                {5}, {8}, 42, contracts::InstanceChange::Stopped, "demo_player"});
            assert(!visible("music_visible") && !visible("running_visible"));
            module.Deliver(contracts::InstanceUpdate{
                {5}, {9}, 43, contracts::InstanceChange::Running, "demo_player"});
            module.Deliver(contracts::InstanceUpdate{
                {5}, {10}, 44, contracts::InstanceChange::Running, "demo_player"});
            module.Deliver(contracts::InstanceUpdate{
                {5}, {9}, 43, contracts::InstanceChange::Stopped, "demo_player"});
            assert(visible("music_visible")); // Another real Music instance remains.
            module.Deliver(contracts::InstanceUpdate{
                {5}, {10}, 44, contracts::InstanceChange::Stopped, "demo_player"});
            module.Deliver(contracts::InstanceUpdate{
                {5}, {9}, 43, contracts::InstanceChange::Running, "demo_settings"});
            assert(visible("settings_visible") && visible("running_visible"));
            module.Disconnected();
            assert(!visible("music_visible") && !visible("settings_visible") &&
                   !visible("running_visible"));
            assert(!bindings.contains("running_badge") && !bindings.contains("launch_status"));
        }
        if (n == 4) {
            auto memory = std::get<std::string>(bindings.at("mem_usage_text"));
            assert(memory.find("GiB") != std::string::npos);
            assert(!bindings.contains("window_tint") && !bindings.contains("theme_dark"));
            assert(theme_request == 1 && selected_theme.empty());
            assert(std::get<double>(bindings.at("memory_usage")) >= 0 &&
                   std::get<double>(bindings.at("memory_usage")) <= 1);
            module.Deliver(contracts::ThemeEvent{1, 1, contracts::ThemeStatus::Current, "glass",
                                                 "Prism Glass"});
            assert(std::get<std::string>(bindings.at("theme_status")) == "");
            assert(std::get<bool>(bindings.at("theme_glass_selected")));
            module.Action("theme:transparent");
            assert(selected_theme == "transparent" && theme_request == 2);
            assert(std::get<std::string>(bindings.at("theme_status")) == "");
            assert(std::get<bool>(bindings.at("theme_glass_selected")) &&
                   !std::get<bool>(bindings.at("theme_transparent_selected")));
            module.Deliver(contracts::ThemeEvent{2, 1, contracts::ThemeStatus::Rejected, "glass",
                                                 "Prism Glass", "invalid"});
            assert(std::get<bool>(bindings.at("theme_error_visible")));
            assert(!std::get<std::string>(bindings.at("theme_status")).empty());
            assert(std::get<bool>(bindings.at("theme_glass_selected")));
            module.Action("theme:square");
            module.Deliver(contracts::ThemeEvent{3, 2, contracts::ThemeStatus::Applied, "square",
                                                 "Prism Square"});
            assert(std::get<std::string>(bindings.at("theme_status")) == "");
            assert(std::get<bool>(bindings.at("theme_square_selected")) &&
                   !std::get<bool>(bindings.at("theme_glass_selected")));
            module.Deliver(contracts::ThemeEvent{0, 1, contracts::ThemeStatus::Current, "glass",
                                                 "Prism Glass"});
            assert(std::get<std::string>(bindings.at("theme_status")) == "");
            assert(std::get<bool>(bindings.at("theme_square_selected")) &&
                   !std::get<bool>(bindings.at("theme_glass_selected")));
            assert(!std::get<bool>(bindings.at("theme_error_visible")));
            assert(std::get<bool>(bindings.at("scheme_dark_selected")));
            module.Action("appearance:light");
            assert(selected_scheme == "light" && theme_request == 4);
            assert(std::get<bool>(
                bindings.at("scheme_dark_selected"))); // Wait for platform confirmation.
            module.Deliver(contracts::ThemeEvent{
                4, 3, contracts::ThemeStatus::Applied, "square", "Prism Square", {}, "light"});
            assert(std::get<bool>(bindings.at("scheme_light_selected")));
            assert(!std::get<bool>(bindings.at("scheme_dark_selected")));
            assert(std::get<bool>(bindings.at("theme_square_selected")));
            module.Action("page:appearance");
            assert(std::get<bool>(bindings.at("page_appearance")));
            assert(!std::get<bool>(bindings.at("page_performance")));
            module.Action("page:performance");
            assert(std::get<bool>(bindings.at("page_performance")));
            assert(std::get<bool>(bindings.at("monitoring_active")));
            module.Action("monitor:toggle");
            assert(!std::get<bool>(bindings.at("monitoring_active")));
            module.Tick(sdk::MonotonicNs() +
                        5000000000ULL); // Consume a previously queued one-shot tick.
            assert(module.TimeoutMs(sdk::MonotonicNs(), 10000) == 10000);
            module.Action("sampling:500");
            assert(std::get<bool>(bindings.at("interval_500_selected")));
            assert(module.TimeoutMs(sdk::MonotonicNs(), 10000) == 10000);
            module.Action("monitor:toggle");
            assert(std::get<bool>(bindings.at("monitoring_active")));
            assert(module.TimeoutMs(sdk::MonotonicNs(), 10000) <= 500);
            module.Action("sampling:2000");
            assert(std::get<bool>(bindings.at("interval_2000_selected")));
            assert(module.TimeoutMs(sdk::MonotonicNs(), 10000) > 1500);
            module.Action("sys:refresh");
        }
    }
}
