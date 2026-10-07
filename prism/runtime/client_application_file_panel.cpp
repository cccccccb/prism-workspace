#include "client_application_p.hpp"

#include "prism/contracts/rounded_region.hpp"
#include "prism/runtime/text_buffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace prism::sdk {
namespace {
template <std::size_t Count>
runtime::TextLayoutInfo VisibleLayout(const runtime::Scene &scene,
                                      const std::array<std::string_view, Count> &regions)
{
    for (const auto region : regions) {
        if (const auto layout = scene.TextLayoutInRegion(region)) {
            if (std::isfinite(layout->width) && layout->width > 0 &&
                std::isfinite(layout->font_size) && layout->font_size > 0 &&
                std::isfinite(layout->height) && layout->height > 0) {
                return *layout;
            }
        }
    }
    throw std::invalid_argument("File panel text has no readable layout");
}

std::string WrappedText(std::string_view text, const runtime::TextLayoutInfo &layout,
                        const runtime::ShapeText &shape)
{
    std::string wrapped;
    while (!text.empty()) {
        auto length = std::min(text.size(), contracts::kMaxOwnerTaskMessageBytes);
        while (length < text.size() && length &&
               (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) {
            --length;
        }
        const auto part = runtime::WrapOwnerTaskText(text.substr(0, length), layout.width,
                                                     layout.font_size, shape);
        wrapped += part;
        text.remove_prefix(length);
        if (!text.empty()) {
            wrapped.push_back('\n');
        }
    }
    std::size_t start = 0;
    while (start < wrapped.size()) {
        const auto end = wrapped.find('\n', start);
        const auto line = std::string_view(wrapped).substr(
            start, end == std::string::npos ? wrapped.size() - start : end - start);
        const auto measured = shape(line, layout.font_size);
        if (!std::isfinite(measured.width) || !std::isfinite(measured.height) ||
            measured.width < 0 || (!line.empty() && measured.height <= 0) ||
            measured.height > layout.height + 0.001 || measured.width > layout.width + 0.001) {
            throw std::invalid_argument("File panel text exceeds its readable width");
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return wrapped;
}

bool SemanticChange(const runtime::OwnerFilePanelView &before,
                    const runtime::OwnerFilePanelView &after)
{
    return before.rows != after.rows || before.directory != after.directory ||
           before.page_caption != after.page_caption || before.overwrite != after.overwrite ||
           before.loading != after.loading;
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

bool ClipContainsBounds(const runtime::InputSnapshotNode &clip,
                        const contracts::LogicalRect &bounds)
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
        const auto &points = clip.contour->points;
        for (std::size_t index = 0; index < points.size(); ++index) {
            if (SegmentEntersBounds(points[index], points[(index + 1) % points.size()], bounds)) {
                return false;
            }
        }
    }
    return true;
}

void ValidateControlGeometry(const runtime::InputSnapshot &input, std::string_view action,
                             double minimum_width)
{
    bool found = false;
    const contracts::LogicalRect viewport{0, 0, input.viewport.width, input.viewport.height};
    for (const auto &node : input.nodes) {
        if (!node.visible || node.action != action) {
            continue;
        }
        const auto *parent = input.Find(node.parent);
        if (found || node.bounds.width + 0.001 < minimum_width || node.bounds.height < 32 ||
            !ContainsBounds(viewport, node.bounds) || !parent ||
            !ContainsBounds(parent->bounds, node.bounds)) {
            throw std::invalid_argument("File panel control does not fit its layout");
        }
        for (; parent; parent = input.Find(parent->parent)) {
            if (parent->clip && !ClipContainsBounds(*parent, node.bounds)) {
                throw std::invalid_argument("File panel control is clipped");
            }
        }
        found = true;
    }
    if (!found) {
        throw std::invalid_argument("File panel control is unavailable");
    }
}

void ValidateFilePanelGeometry(const runtime::InputSnapshot &input,
                               const runtime::OwnerFilePanelView &view)
{
    if (!std::isfinite(input.viewport.width) || !std::isfinite(input.viewport.height) ||
        input.viewport.width < 320 || input.viewport.height < 240) {
        throw std::invalid_argument("File panel requires at least a 320 by 240 viewport");
    }
    ValidateControlGeometry(input, runtime::kOwnerTaskCancelAction, 72);
    ValidateControlGeometry(input, runtime::kOwnerFileUpAction, 48);
    if (view.show_filename) {
        ValidateControlGeometry(input, runtime::kOwnerFileNameAction, 32);
    }
    if (view.overwrite) {
        ValidateControlGeometry(input, runtime::kOwnerFileBackAction, 64);
        ValidateControlGeometry(input, runtime::kOwnerFileReplaceAction, 88);
    } else {
        ValidateControlGeometry(input, runtime::kOwnerFileSubmitAction, 88);
    }
}
} // namespace

runtime::Blueprint ClientApplication::Impl::ComposeOwnerPanels(runtime::Blueprint blueprint) const
{
    runtime::ValidateOwnerFeedbackApplication(blueprint);
    if (owner_feedback_panel_template) {
        blueprint = runtime::ComposeOwnerFeedbackPanel(
            std::move(blueprint), runtime::LinkComponent(*owner_feedback_panel_template));
    }
    std::optional<runtime::Blueprint> confirmation;
    std::optional<runtime::Blueprint> file;
    if (owner_task_panel_template) {
        confirmation = runtime::LinkComponent(*owner_task_panel_template);
    }
    if (owner_file_panel_template) {
        file = runtime::LinkComponent(*owner_file_panel_template);
    }
    return runtime::ComposeOwnerTaskPanels(
        std::move(blueprint), confirmation ? &*confirmation : nullptr, file ? &*file : nullptr);
}

runtime::BindingValues ClientApplication::Impl::OwnerPanelDefaults() const
{
    auto values = runtime::OwnerTaskPanelDefaults();
    if (owner_file_panel_template) {
        for (const auto &[key, value] : runtime::OwnerFilePanelDefaults()) {
            values.insert_or_assign(key, value);
        }
    }
    if (owner_feedback_panel_template) {
        for (const auto &[key, value] : runtime::OwnerFeedbackPanelDefaults()) {
            values.insert_or_assign(key, value);
        }
    }
    return values;
}

bool ClientApplication::ConfigureOwnerFilePanel(const runtime::PreparedComponent &prepared)
{
    auto &app = *impl_;
    if (!prepared || !prepared.Images().empty() || app.closed || app.failed || app.install ||
        app.owner_task_scope || app.owner_confirmation || app.owner_file_view ||
        app.owner_file_panel_template) {
        return false;
    }

    runtime::Blueprint root;
    const auto tree = runtime::LinkComponent(prepared);
    runtime::ComposeOwnerTaskPanels(std::move(root), nullptr, &tree);
    app.owner_file_panel_template = prepared;
    return true;
}

bool ClientApplication::SupportsOwnerFileTasks() const noexcept
{
    try {
        const auto &app = *impl_;
        const auto size = app.ui_configure_count
                              ? app.ui_metrics.logical_size
                              : contracts::LogicalSize{static_cast<double>(app.config.width),
                                                       static_cast<double>(app.config.height)};
        return !app.closed && !app.failed && !app.owner_tasks_retired && app.opened_once &&
               app.installed_ui.owner && app.owner_file_panel_template && app.scene &&
               app.bridge->terminal.Reason() == runtime::TerminalReason::None &&
               runtime::HasOwnerTaskPanel(*app.scene) &&
               app.scene->RegionId(runtime::kOwnerFileTitleRegions.front()) && size.width >= 320 &&
               size.height >= 240;
    } catch (...) {
        return false;
    }
}

void ClientApplication::Impl::ClearOwnerFilePanel()
{
    if (owner_file_view && scene && owner_file_ui == installed_ui) {
        scene->SetBinding("__prism_task_visible", false);
        scene->SetBinding("__prism_task_file_mode", false);
    }
    owner_file_view.reset();
    owner_file_kind.reset();
    owner_file_ui = {};
    owner_task_bindings.clear();
}

void ClientApplication::Impl::UpdateOwnerFilePanelText()
{
    if (!owner_file_view || !scene || owner_file_ui != installed_ui) {
        return;
    }

    try {
        scene->ResolveLayout();
        const auto input = scene->CaptureInputSnapshot();
        ValidateFilePanelGeometry(*input, *owner_file_view);
        const runtime::ShapeText shape = std::bind_front(&Impl::ShapeText, this);
        const auto title = VisibleLayout(*scene, runtime::kOwnerFileTitleRegions);
        const auto status = VisibleLayout(*scene, runtime::kOwnerFileStatusRegions);
        const auto detail = VisibleLayout(*scene, runtime::kOwnerFileDetailRegions);
        const auto wrapped_title = WrappedText(owner_file_view->title, title, shape);
        const auto wrapped_status = WrappedText(owner_file_view->status, status, shape);
        const auto wrapped_detail = WrappedText(owner_file_view->selected_caption, detail, shape);
        owner_task_bindings.insert_or_assign("__prism_task_file_title", wrapped_title);
        owner_task_bindings.insert_or_assign("__prism_task_file_status", wrapped_status);
        owner_task_bindings.insert_or_assign("__prism_task_file_selected_caption", wrapped_detail);
        scene->SetBinding("__prism_task_file_title", wrapped_title);
        scene->SetBinding("__prism_task_file_status", wrapped_status);
        scene->SetBinding("__prism_task_file_selected_caption", wrapped_detail);
        scene->ResolveLayout();
    } catch (...) {
        if (!owner_task_scope) {
            throw;
        }
        if (owner_tasks) {
            owner_tasks->Fail(owner_task_scope->identity,
                              {runtime::TaskFailureCode::PreparationFailed,
                               "File task controls or text do not fit the current layout"});
        }
        RevokeOwnerTaskScope();
    }
}

std::optional<runtime::TaskIdentity>
ClientApplication::BeginOwnerFileTask(const contracts::OwnerTaskRequest &request,
                                      const runtime::OwnerFilePanelView &view)
{
    auto &app = *impl_;
    if (!SupportsOwnerFileTasks() || request.kind == contracts::OwnerTaskKind::Confirmation ||
        !contracts::ValidateOwnerTaskRequest(request) ||
        !runtime::ValidateOwnerFilePanelView(view) ||
        view.show_filename != (request.kind == contracts::OwnerTaskKind::SaveFile) ||
        ActiveOwnerTask() || app.owner_confirmation || app.owner_file_view) {
        return std::nullopt;
    }
    if (app.owner_file_generation == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    const auto generation = ++app.owner_file_generation;
    app.owner_file_view = view;
    app.owner_file_kind = request.kind;
    app.owner_file_ui = app.installed_ui;
    try {
        app.owner_task_bindings = runtime::OwnerFilePanelBindings(request.kind, view);
        for (const auto &[key, value] : app.owner_task_bindings) {
            app.scene->SetBinding(key, value);
        }
        app.UpdateOwnerFilePanelText();
        const auto identity = BeginOwnerTask(runtime::kOwnerTaskPanelRegion);
        if (!identity && generation == app.owner_file_generation) {
            app.ClearOwnerFilePanel();
        }
        return identity;
    } catch (...) {
        if (generation == app.owner_file_generation) {
            app.ClearOwnerFilePanel();
        }
        throw;
    }
}

bool ClientApplication::UpdateOwnerFileTask(runtime::TaskIdentity identity,
                                            const runtime::OwnerFilePanelView &view)
{
    auto &app = *impl_;
    app.ReconcileOwnerTask();
    const auto active = ActiveOwnerTask();
    if (!active || active->identity != identity || !app.owner_file_view || !app.owner_file_kind ||
        app.owner_file_ui != app.installed_ui || !runtime::ValidateOwnerFilePanelView(view)) {
        return false;
    }
    if (view.show_filename != (*app.owner_file_kind == contracts::OwnerTaskKind::SaveFile)) {
        return false;
    }

    auto values = runtime::OwnerFilePanelBindings(*app.owner_file_kind, view);
    const auto refresh = SemanticChange(*app.owner_file_view, view);
    app.owner_file_view = view;
    app.owner_task_bindings = std::move(values);
    for (const auto &[key, value] : app.owner_task_bindings) {
        app.scene->SetBinding(key, value);
    }
    app.UpdateOwnerFilePanelText();
    if (!app.owner_file_view) {
        return false;
    }
    if (refresh) {
        return RefreshOwnerTask(identity);
    }
    app.PublishOwnerTaskChange();
    return app.owner_file_view && app.owner_task_scope &&
           app.owner_task_scope->identity == identity;
}
} // namespace prism::sdk
