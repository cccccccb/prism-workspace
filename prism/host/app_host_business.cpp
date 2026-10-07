#include "app_host_p.hpp"
#include "prism/runtime/owner_feedback_panel.hpp"
#include "prism/runtime/owner_task_panel.hpp"

namespace prism::sdk {
using namespace host_detail;

bool AppHost::Impl::SetBinding(std::string_view key, runtime::PropertyValue value)
{
    if (runtime::IsOwnerTaskReservedName(key) || runtime::IsOwnerFeedbackReservedName(key)) {
        return false;
    }

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

bool AppHost::Impl::HandleCloseRequested()
{
    return !business || business->RequestClose();
}

void AppHost::Impl::AdvanceCloseDecision()
{
    if (!business || failed || closed) {
        return;
    }
    const auto decision = business->ConsumeCloseDecision();
    if (!decision) {
        return;
    }

    business_progress = true;
    if (*decision && !frontend->AcceptClose()) {
        Fail(contracts::LaunchError::RuntimeFailed, "Accepted close command unavailable");
    }
}

void AppHost::Impl::HandleControlValue(const runtime::ControlEdit &edit)
{
    if (business) {
        business->ControlValue(edit);
    }
}

void AppHost::Impl::HandleTextEdit(std::string_view action, std::string_view text)
{
    if (HandleFileTaskText(action, text) || runtime::IsOwnerTaskReservedName(action) ||
        runtime::IsOwnerFeedbackReservedName(action)) {
        return;
    }
    if (business) {
        business->TextEdit(action, text);
    }
}

void AppHost::Impl::HandleAction(std::string_view action)
{
    if (!HandleOwnerTaskAction(action) && !runtime::IsOwnerFeedbackReservedName(action) &&
        business) {
        business->Action(action);
    }
}

void AppHost::Impl::HandleGesture(const contracts::GestureEvent &event)
{
    if (business) {
        business->Gesture(event);
    }
}

void AppHost::Impl::PrepareBusiness()
{
    prepared_business = std::make_unique<ModuleSession>(
        package->module, package->manifest.app_id, config.instance.value,
        std::bind_front(&Impl::SetBinding, this), std::bind_front(&Impl::LaunchApplication, this),
        std::bind_front(&Impl::SubscribeInstances, this), std::bind_front(&Impl::SelectTheme, this),
        std::bind_front(&Impl::SelectColorScheme, this), scheduler, config.module_limits,
        package->assets, config.subscribe_layout, config.submit_layout_control,
        std::bind_front(&Impl::RequestOwnerTask, this),
        std::bind_front(&Impl::CancelOwnerTaskRequest, this),
        std::bind_front(&Impl::OwnerTaskCapabilities, this),
        std::bind_front(&Impl::ShowOwnerFeedback, this),
        std::bind_front(&Impl::DismissOwnerFeedback, this),
        std::bind_front(&Impl::OwnerFeedbackCapabilities, this));
    const auto feedback_source =
        LoadUiSource("owner-feedback-panel.prism", "resources/ui/owner-feedback-panel.prism");
    if (feedback_source) {
        frontend->ConfigureOwnerFeedbackPanel(runtime::PrepareComponent(
            *feedback_source, {"prism.owner-feedback-panel", "owner-feedback-panel.prism", {}}));
    }
    if (prepared_business->SupportsOwnerTasks()) {
        const auto source =
            LoadUiSource("owner-task-panel.prism", "resources/ui/owner-task-panel.prism");
        if (source) {
            frontend->ConfigureOwnerTaskPanel(runtime::PrepareComponent(
                *source, {"prism.owner-task-panel", "owner-task-panel.prism", {}}));
        }
        const auto file_source =
            LoadUiSource("owner-file-panel.prism", "resources/ui/owner-file-panel.prism");
        if (file_source) {
            frontend->ConfigureOwnerFilePanel(runtime::PrepareComponent(
                *file_source, {"prism.owner-file-panel", "owner-file-panel.prism", {}}));
        }
    }
}

bool AppHost::Impl::StartBusiness()
{
    business = std::move(prepared_business);
    const bool started = business->Start();
    startup.module_load_us = business->LoadDurationNs() / 1000;
    startup.module_create_us = business->CreateDurationNs() / 1000;
    if (!started) {
        return Fail(contracts::LaunchError::RuntimeFailed, business->StartDiagnostic());
    }

    frontend->OnCloseRequested(std::bind_front(&Impl::HandleCloseRequested, this));
    frontend->OnControlValue(std::bind_front(&Impl::HandleControlValue, this));
    frontend->OnTextEdit(std::bind_front(&Impl::HandleTextEdit, this));
    frontend->OnAction(std::bind_front(&Impl::HandleAction, this));
    frontend->OnGesture(std::bind_front(&Impl::HandleGesture, this));

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
