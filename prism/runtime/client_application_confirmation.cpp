#include "client_application_p.hpp"
#include "prism/runtime/owner_task_panel.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace prism::sdk {
namespace {
template <std::size_t Count>
void ResetTextScroll(runtime::Scene &scene, const runtime::InputSnapshot &input,
                     const std::array<std::string_view, Count> &regions)
{
    for (const auto region : regions) {
        const auto id = scene.RegionId(region);
        if (id && id.index < input.nodes.size() && input.nodes[id.index].id == id) {
            scene.ScrollTo(input.nodes[id.index].parent, 0);
        }
    }
}

void ValidateWrappedText(std::string_view text, const runtime::TextLayoutInfo &layout,
                         const runtime::ShapeText &shape)
{
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto line =
            text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        const auto metrics = shape(line, layout.font_size);
        if (!std::isfinite(metrics.width) || !std::isfinite(metrics.height) || metrics.width < 0 ||
            metrics.height < 0 || (!line.empty() && metrics.height == 0) ||
            metrics.width > layout.width + 0.001 || metrics.height > layout.height + 0.001) {
            throw std::invalid_argument("Confirmation line exceeds available width");
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
}

template <std::size_t Count>
runtime::TextLayoutInfo VisibleTextLayout(const runtime::Scene &scene,
                                          const std::array<std::string_view, Count> &regions)
{
    for (const auto region : regions) {
        if (const auto metrics = scene.TextLayoutInRegion(region)) {
            if (std::isfinite(metrics->width) && metrics->width > 0 &&
                std::isfinite(metrics->font_size) && metrics->font_size > 0) {
                return *metrics;
            }
        }
    }
    throw std::invalid_argument("Confirmation text has no readable layout");
}
} // namespace

bool ClientApplication::ConfigureOwnerTaskPanel(const runtime::PreparedComponent &prepared)
{
    auto &app = *impl_;
    if (!prepared || !prepared.Images().empty() || app.closed || app.failed || app.install ||
        app.owner_task_scope || app.owner_confirmation || app.owner_task_panel_template) {
        return false;
    }

    runtime::Blueprint test_root;
    runtime::ComposeOwnerTaskPanel(std::move(test_root), runtime::LinkComponent(prepared));
    app.owner_task_panel_template = prepared;
    return true;
}

bool ClientApplication::SupportsOwnerConfirmation() const noexcept
{
    try {

        const auto &app = *impl_;
        const auto size = app.ui_configure_count
                              ? app.ui_metrics.logical_size
                              : contracts::LogicalSize{static_cast<double>(app.config.width),
                                                       static_cast<double>(app.config.height)};
        return !app.closed && !app.failed && !app.owner_tasks_retired && app.opened_once &&
               app.installed_ui.owner && app.owner_task_panel_template && app.scene &&
               app.bridge->terminal.Reason() == runtime::TerminalReason::None &&
               runtime::HasOwnerTaskPanel(*app.scene) && size.width >= 240 && size.height >= 180;
    } catch (...) {
        return false;
    }
}

void ClientApplication::Impl::ClearOwnerConfirmation()
{
    if (owner_confirmation && scene && owner_confirmation_ui == installed_ui) {
        scene->SetBinding("__prism_task_visible", false);
    }
    owner_confirmation.reset();
    owner_task_bindings.clear();
    owner_confirmation_ui = {};
}

void ClientApplication::Impl::UpdateOwnerConfirmationText()
{
    if (!owner_confirmation || !scene || owner_confirmation_ui != installed_ui) {
        return;
    }

    try {
        scene->ResolveLayout();
        const auto body = VisibleTextLayout(*scene, runtime::kOwnerTaskBodyRegions);
        const auto title = VisibleTextLayout(*scene, runtime::kOwnerTaskTitleRegions);
        const runtime::ShapeText shape = std::bind_front(&Impl::ShapeText, this);
        const auto wrapped_title = runtime::WrapOwnerTaskText(owner_confirmation->title,
                                                              title.width, title.font_size, shape);
        const auto wrapped_body = runtime::WrapOwnerTaskText(owner_confirmation->message,
                                                             body.width, body.font_size, shape);
        ValidateWrappedText(wrapped_title, title, shape);
        ValidateWrappedText(wrapped_body, body, shape);
        for (std::size_t index = 0; index < owner_confirmation->choices.size(); ++index) {
            const auto label =
                VisibleTextLayout(*scene, runtime::kOwnerTaskChoiceLabelRegions[index]);
            const auto shaped = shape(owner_confirmation->choices[index].label, label.font_size);
            if (!std::isfinite(shaped.width) || !std::isfinite(shaped.height) ||
                shaped.width <= 0 || shaped.height <= 0 || shaped.width > label.width ||
                shaped.height > label.height) {
                throw std::invalid_argument("Confirmation choice label exceeds available width");
            }
        }

        owner_task_bindings.insert_or_assign("__prism_task_title", wrapped_title);
        owner_task_bindings.insert_or_assign("__prism_task_message", wrapped_body);
        scene->SetBinding("__prism_task_title", wrapped_title);
        scene->SetBinding("__prism_task_message", wrapped_body);
        scene->ResolveLayout();
    } catch (...) {
        if (!owner_task_scope) {
            throw;
        }
        if (owner_tasks) {
            owner_tasks->Fail(owner_task_scope->identity,
                              {runtime::TaskFailureCode::PreparationFailed,
                               "Confirmation text does not fit the current layout"});
        }
        RevokeOwnerTaskScope();
    }
}

std::optional<runtime::TaskIdentity>
ClientApplication::BeginOwnerConfirmation(const contracts::OwnerTaskRequest &request)
{
    auto &app = *impl_;
    if (!SupportsOwnerConfirmation() || request.kind != contracts::OwnerTaskKind::Confirmation ||
        !contracts::ValidateOwnerTaskRequest(request) || ActiveOwnerTask() ||
        app.owner_confirmation) {
        return std::nullopt;
    }

    if (app.owner_confirmation_generation == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    const auto generation = ++app.owner_confirmation_generation;
    app.owner_confirmation = request;
    app.owner_confirmation_ui = app.installed_ui;
    try {
        app.owner_task_bindings = runtime::OwnerTaskPanelBindings(request);
        app.owner_task_bindings.insert_or_assign(
            "__prism_task_context",
            (app.config.title.empty() ? app.config.app_id : app.config.title) + " / Confirmation");
        for (const auto &[key, value] : app.owner_task_bindings) {
            app.scene->SetBinding(key, value);
        }
        app.UpdateOwnerConfirmationText();
        const auto input = app.scene->CaptureInputSnapshot();
        ResetTextScroll(*app.scene, *input, runtime::kOwnerTaskBodyRegions);
        ResetTextScroll(*app.scene, *input, runtime::kOwnerTaskTitleRegions);
        const auto identity = BeginOwnerTask(runtime::kOwnerTaskPanelRegion);
        if (!identity && generation == app.owner_confirmation_generation) {
            app.ClearOwnerConfirmation();
        }
        return identity;
    } catch (...) {
        if (generation == app.owner_confirmation_generation) {
            app.ClearOwnerConfirmation();
        }
        throw;
    }
}
} // namespace prism::sdk
