#include "client_application_p.hpp"
#include "prism/contracts/rounded_region.hpp"
#include "prism/runtime/owner_task_panel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace prism::sdk {
namespace {
runtime::TextLayoutInfo TextLayout(const runtime::Scene &scene, std::string_view region)
{
    const auto layout = scene.TextLayoutInRegion(region);
    if (!layout || !std::isfinite(layout->width) || layout->width <= 0 ||
        !std::isfinite(layout->height) || layout->height <= 0 ||
        !std::isfinite(layout->font_size) || layout->font_size <= 0) {
        throw std::invalid_argument("Feedback text has no readable layout");
    }
    return *layout;
}

std::string WrapText(std::string_view text, const runtime::TextLayoutInfo &layout,
                     const runtime::ShapeText &shape)
{
    auto wrapped = runtime::WrapOwnerTaskText(text, layout.width, layout.font_size, shape);
    std::size_t start = 0;
    while (start < wrapped.size()) {
        const auto end = wrapped.find('\n', start);
        const auto line = std::string_view(wrapped).substr(
            start, end == std::string::npos ? wrapped.size() - start : end - start);
        const auto metrics = shape(line, layout.font_size);
        if (!std::isfinite(metrics.width) || !std::isfinite(metrics.height) || metrics.width < 0 ||
            metrics.height < 0 || (!line.empty() && metrics.height == 0) ||
            metrics.width > layout.width + 0.001 || metrics.height > layout.height + 0.001) {
            throw std::invalid_argument("Feedback line exceeds its readable layout");
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return wrapped;
}

bool ContainsBounds(const contracts::LogicalRect &outer, const contracts::LogicalRect &inner)
{
    return std::isfinite(inner.x) && std::isfinite(inner.y) && std::isfinite(inner.width) &&
           std::isfinite(inner.height) && inner.width > 0 && inner.height > 0 &&
           inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width + 0.001 &&
           inner.y + inner.height <= outer.y + outer.height + 0.001;
}

bool SegmentEntersBounds(contracts::LogicalPoint a, contracts::LogicalPoint b,
                         const contracts::LogicalRect &bounds)
{
    double first = 0;
    double last = 1;
    const std::array origins{a.x, a.y};
    const std::array deltas{b.x - a.x, b.y - a.y};
    const std::array low{bounds.x + 0.001, bounds.y + 0.001};
    const std::array high{bounds.x + bounds.width - 0.001, bounds.y + bounds.height - 0.001};
    for (std::size_t axis = 0; axis < origins.size(); ++axis) {
        if (deltas[axis] == 0) {
            if (origins[axis] <= low[axis] || origins[axis] >= high[axis]) {
                return false;
            }
            continue;
        }
        const auto start = (low[axis] - origins[axis]) / deltas[axis];
        const auto end = (high[axis] - origins[axis]) / deltas[axis];
        first = std::max(first, std::min(start, end));
        last = std::min(last, std::max(start, end));
        if (first >= last) {
            return false;
        }
    }
    return first < last;
}

bool ClipContains(const runtime::InputSnapshotNode &clip, const contracts::LogicalRect &bounds)
{
    if (!ContainsBounds(clip.bounds, bounds)) {
        return false;
    }
    const std::array corners{
        contracts::LogicalPoint{bounds.x, bounds.y},
        contracts::LogicalPoint{bounds.x + bounds.width, bounds.y},
        contracts::LogicalPoint{bounds.x, bounds.y + bounds.height},
        contracts::LogicalPoint{bounds.x + bounds.width, bounds.y + bounds.height}};
    for (const auto point : corners) {
        if (clip.contour
                ? !contracts::ContourContains(*clip.contour, point)
                : !contracts::RoundedRegionContains(point, {clip.bounds, clip.radius}, true)) {
            return false;
        }
    }
    if (clip.contour) {
        for (std::size_t i = 0; i < clip.contour->points.size(); ++i) {
            if (SegmentEntersBounds(clip.contour->points[i],
                                    clip.contour->points[(i + 1) % clip.contour->points.size()],
                                    bounds)) {
                return false;
            }
        }
    }
    return true;
}

void ValidateControl(const runtime::InputSnapshot &input, std::string_view action)
{
    bool found = false;
    const contracts::LogicalRect viewport{0, 0, input.viewport.width, input.viewport.height};
    for (const auto &node : input.nodes) {
        if (!node.visible || node.action != action) {
            continue;
        }
        const auto *parent = input.Find(node.parent);
        if (found || node.bounds.width < 32 || node.bounds.height < 32 ||
            !ContainsBounds(viewport, node.bounds) || !parent ||
            !ContainsBounds(parent->bounds, node.bounds)) {
            throw std::invalid_argument("Feedback control does not fit its layout");
        }
        for (; parent; parent = input.Find(parent->parent)) {
            if (parent->clip && !ClipContains(*parent, node.bounds)) {
                throw std::invalid_argument("Feedback control is clipped");
            }
        }
        found = true;
    }
    if (!found) {
        throw std::invalid_argument("Feedback control is unavailable");
    }
}

void ProjectFeedback(runtime::Scene &scene, const contracts::OwnerFeedbackRequest &request,
                     runtime::BindingValues &values, const runtime::ShapeText &shape,
                     std::uint64_t generation)
{
    scene.ResolveLayout();
    const auto title = TextLayout(scene, runtime::kOwnerFeedbackTitleRegion);
    const auto body = TextLayout(scene, runtime::kOwnerFeedbackMessageRegion);
    values.insert_or_assign("__prism_feedback_title", WrapText(request.title, title, shape));
    values.insert_or_assign("__prism_feedback_message", WrapText(request.message, body, shape));
    for (std::size_t i = 0; i < request.actions.size(); ++i) {
        const auto label = TextLayout(scene, runtime::kOwnerFeedbackLabelRegions[i]);
        const auto metrics = shape(request.actions[i].label, label.font_size);
        if (!std::isfinite(metrics.width) || !std::isfinite(metrics.height) || metrics.width <= 0 ||
            metrics.height <= 0 || metrics.width > label.width || metrics.height > label.height) {
            throw std::invalid_argument("Feedback action label exceeds its readable layout");
        }
    }
    for (const auto &[key, value] : values) {
        scene.SetBinding(key, value);
    }
    scene.ResolveLayout();
    const auto input = scene.CaptureInputSnapshot();
    if (input->viewport.width < 240 || input->viewport.height < 180) {
        throw std::invalid_argument("Feedback requires at least a 240 by 180 viewport");
    }
    ValidateControl(*input, runtime::OwnerFeedbackActionName(generation, request.request_id, 0));
    for (const auto &action : request.actions) {
        ValidateControl(
            *input, runtime::OwnerFeedbackActionName(generation, request.request_id, action.id));
    }
}
} // namespace

bool ClientApplication::ConfigureOwnerFeedbackPanel(const runtime::PreparedComponent &prepared)
{
    auto &app = *impl_;
    if (!prepared || !prepared.Images().empty() || app.closed || app.failed || app.install ||
        app.owner_feedback_retired || app.owner_feedback_panel_template || app.owner_feedback) {
        return false;
    }
    try {
        runtime::Blueprint root;
        runtime::ComposeOwnerFeedbackPanel(std::move(root), runtime::LinkComponent(prepared));
        app.owner_feedback_panel_template = prepared;
        return true;
    } catch (...) {
        return false;
    }
}

bool ClientApplication::SupportsOwnerFeedback() const noexcept
{
    try {
        const auto &app = *impl_;
        const auto size = app.ui_configure_count
                              ? app.ui_metrics.logical_size
                              : contracts::LogicalSize{static_cast<double>(app.config.width),
                                                       static_cast<double>(app.config.height)};
        return !app.closed && !app.failed && !app.owner_feedback_retired && app.opened_once &&
               app.installed_ui.owner && app.owner_feedback_panel_template && app.scene &&
               app.bridge->terminal.Reason() == runtime::TerminalReason::None &&
               runtime::HasOwnerFeedbackPanel(*app.scene) && size.width >= 240 &&
               size.height >= 180;
    } catch (...) {
        return false;
    }
}

bool ClientApplication::ShowOwnerFeedback(const contracts::OwnerFeedbackRequest &request)
{
    auto &app = *impl_;
    if (!SupportsOwnerFeedback() || !contracts::ValidateOwnerFeedbackRequest(request) ||
        app.owner_feedback_action || app.owner_feedback_generation == UINT64_MAX) {
        return false;
    }
    const auto generation = app.owner_feedback_generation + 1;
    bool committed = false;
    try {
        const auto shared = runtime::LinkComponent(*app.owner_feedback_panel_template);
        const std::array updates{runtime::RegionUpdate{
            std::string(runtime::kOwnerFeedbackPanelRegion),
            runtime::InstantiateOwnerFeedbackPanel(shared, request, generation)}};
        auto values = app.binding_values;
        for (const auto &[key, value] : app.OwnerPanelDefaults()) {
            values.insert_or_assign(key, value);
        }
        for (const auto &[key, value] : app.owner_task_bindings) {
            values.insert_or_assign(key, value);
        }
        auto feedback = runtime::OwnerFeedbackPanelBindings(request);
        for (const auto &[key, value] : feedback) {
            values.insert_or_assign(key, value);
        }
        const auto revision = app.scene->TransactionRevision();
        runtime::Scene candidate(app.scene->ReplacementBlueprint(updates),
                                 std::bind_front(&Impl::ShapeText, &app), app.shaper.FontId(),
                                 app.theme);
        candidate.SetViewport(app.ui_configure_count
                                  ? app.ui_metrics.logical_size
                                  : contracts::LogicalSize{static_cast<double>(app.config.width),
                                                           static_cast<double>(app.config.height)});
        std::string failure;
        if (!candidate.PrepareDetached(values, &failure)) {
            return false;
        }
        ProjectFeedback(candidate, request, feedback, std::bind_front(&Impl::ShapeText, &app),
                        generation);
        const bool hidden = app.owner_task_scope.has_value();
        feedback.insert_or_assign("__prism_feedback_visible", !hidden);
        for (const auto &[key, value] : feedback) {
            values.insert_or_assign(key, value);
        }
        // Copy all owned state before the live transaction. Rejection preserves the old feedback.
        std::optional<contracts::OwnerFeedbackRequest> next(request);
        if (!app.scene->ReplaceRegions(updates, values, candidate, revision, &failure)) {
            return false;
        }
        committed = true;
        app.owner_feedback.swap(next);
        app.owner_feedback_bindings.swap(feedback);
        app.owner_feedback_generation = generation;
        app.owner_feedback_ui = app.installed_ui;
        app.owner_feedback_hidden = hidden;
        app.owner_feedback_wait_adoption = true;
        const auto duration =
            request.kind == contracts::OwnerFeedbackKind::Error
                ? 0u
                : (request.duration_ms ? request.duration_ms
                                       : contracts::kDefaultOwnerFeedbackDurationMs);
        app.owner_feedback_session.Begin(generation,
                                         static_cast<std::uint64_t>(duration) * 1000000);
        app.InvalidateQueuedFrame();
        app.PublishFramePacket();
        app.QueueRenderUpdate(true);
        if (app.bridge->terminal.Reason() != runtime::TerminalReason::None) {
            app.FailFrontend();
            return false;
        }
        return true;
    } catch (...) {
        if (committed) {
            app.bridge->terminal.Fail(runtime::TerminalReason::InternalFailure);
            app.FailFrontend();
        }
        return false;
    }
}

void ClientApplication::Impl::ClearOwnerFeedback(bool clear_pending)
{
    if (owner_feedback && scene && owner_feedback_ui == installed_ui) {
        scene->SetBinding("__prism_feedback_visible", false);
    }
    owner_feedback.reset();
    owner_feedback_bindings.clear();
    owner_feedback_ui = {};
    owner_feedback_hidden = false;
    owner_feedback_wait_adoption = false;
    owner_feedback_session.Clear();
    if (clear_pending) {
        owner_feedback_action.reset();
    }
}

void ClientApplication::Impl::RetireOwnerFeedback()
{
    owner_feedback_retired = true;
    ClearOwnerFeedback(true);
    owner_feedback_pointers.clear();
}

void ClientApplication::RetireOwnerFeedback()
{
    impl_->RetireOwnerFeedback();
}

bool ClientApplication::DismissOwnerFeedback(std::uint64_t request_id)
{
    auto &app = *impl_;
    if (!app.owner_feedback || app.owner_feedback->request_id != request_id) {
        return false;
    }
    app.ClearOwnerFeedback();
    app.InvalidateQueuedFrame();
    app.PublishFramePacket();
    app.QueueRenderUpdate(true);
    return true;
}

std::optional<contracts::OwnerFeedbackAction> ClientApplication::TakeOwnerFeedbackAction()
{
    auto result = impl_->owner_feedback_action;
    impl_->owner_feedback_action.reset();
    return result;
}

void ClientApplication::Impl::UpdateOwnerFeedbackText()
{
    if (!owner_feedback || owner_feedback_ui != installed_ui || !scene || owner_feedback_hidden) {
        return;
    }
    try {
        ProjectFeedback(*scene, *owner_feedback, owner_feedback_bindings,
                        std::bind_front(&Impl::ShapeText, this), owner_feedback_generation);
    } catch (...) {
        ClearOwnerFeedback();
    }
}
} // namespace prism::sdk
