#include "app_host_p.hpp"

namespace prism::sdk {
using namespace host_detail;

bool AppHost::Impl::SetBinding(std::string_view key, runtime::PropertyValue value)
{
    if (installed_plan && !installed_plan->legacy) {
        const auto declaration =
            std::find_if(installed_plan->bindings.begin(), installed_plan->bindings.end(),
                         [&](const runtime::LoadBinding &binding) { return binding.name == key; });
        if (declaration == installed_plan->bindings.end() ||
            !MatchesBinding(declaration->type, value)) {
            return false;
        }
        if (mounted_bindings.contains(key) && !frontend->SetBinding(key, value)) {
            return false;
        }
        bindings.insert_or_assign(std::string(key), std::move(value));
        return true;
    }
    return frontend->SetBinding(key, std::move(value));
}

std::uint64_t AppHost::Impl::LaunchApplication(std::string_view app_id)
{
    if (config.launch_app) {
        return config.launch_app(app_id);
    }
    if (!launches) {
        launches = std::make_unique<LaunchClient>();
    }
    return launches->Launch(std::string(app_id));
}

std::uint64_t AppHost::Impl::SubscribeInstances()
{
    if (config.subscribe_instances) {
        return config.subscribe_instances();
    }
    if (!launches) {
        launches = std::make_unique<LaunchClient>();
    }
    return launches->SubscribeInstances();
}

std::uint64_t AppHost::Impl::SelectTheme(std::string_view id)
{
    if (config.select_theme) {
        return config.select_theme(id);
    }
    if (!launches) {
        launches = std::make_unique<LaunchClient>();
    }
    return launches->SelectTheme(std::string(id));
}

std::uint64_t AppHost::Impl::SelectColorScheme(std::string_view scheme)
{
    if (config.select_color_scheme) {
        return config.select_color_scheme(scheme);
    }
    if (!launches) {
        launches = std::make_unique<LaunchClient>();
    }
    return launches->SelectTheme({}, std::string(scheme));
}

void AppHost::Impl::HandleAction(std::string_view action)
{
    business->Action(action);
}

bool AppHost::Impl::StartBusiness()
{
    business = std::make_unique<ModuleSession>(
        package->module, package->manifest.app_id, config.instance.value,
        std::bind_front(&Impl::SetBinding, this), std::bind_front(&Impl::LaunchApplication, this),
        std::bind_front(&Impl::SubscribeInstances, this), std::bind_front(&Impl::SelectTheme, this),
        std::bind_front(&Impl::SelectColorScheme, this), scheduler, config.module_limits,
        package->assets);

    const bool started = business->Start();
    startup.module_load_us = business->LoadDurationNs() / 1000;
    startup.module_create_us = business->CreateDurationNs() / 1000;
    if (!started) {
        return Fail(contracts::LaunchError::RuntimeFailed, business->StartDiagnostic());
    }

    frontend->OnAction(std::bind_front(&Impl::HandleAction, this));

    if (config.initial_theme) {
        const auto &theme = *config.initial_theme;
        business->Deliver(contracts::ThemeEvent{0,
                                                theme.generation,
                                                contracts::ThemeStatus::Current,
                                                theme.id,
                                                theme.name,
                                                {},
                                                theme.color_scheme});
    }
    return true;
}

void AppHost::Impl::DispatchBusinessWork()
{
    if (!business || business_work_dispatched || failed || closed) {
        return;
    }
    business_work_dispatched = true;
    business_progress |= business->DispatchWork() != 0;
    Observe();
}

void AppHost::Impl::DrainLaunches()
{
    if (!launches || !business) {
        return;
    }
    for (const auto &event : launches->Pump(0)) {
        business->Deliver(event);
    }
    for (const auto &event : launches->TakeInstanceUpdates()) {
        business->Deliver(event);
    }
    for (const auto &event : launches->TakeThemeEvents()) {
        if (event.status != contracts::ThemeStatus::Rejected) {
            std::string diagnostic;
            if (!owner->ApplyTheme(theme::LoadTheme(theme::DefaultThemeRoot(), event.id,
                                                    event.generation, event.color_scheme),
                                   &diagnostic)) {
                business->Deliver(contracts::ThemeEvent{
                    event.request, owner->ThemeGeneration(), contracts::ThemeStatus::Rejected,
                    event.id, event.name, std::move(diagnostic), event.color_scheme});
                continue;
            }
        }
        business->Deliver(event);
    }
    if (launches->Connected()) {
        launch_disconnected = false;
    } else if (!launch_disconnected) {
        launch_disconnected = true;
        business->Disconnected();
    }
}
} // namespace prism::sdk
