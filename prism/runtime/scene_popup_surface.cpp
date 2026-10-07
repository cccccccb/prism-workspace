#include "popup_surface_p.hpp"
#include "scene_p.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
bool IntegerRect(const contracts::LogicalRect &rect)
{
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
           std::isfinite(rect.height) && std::floor(rect.x) == rect.x &&
           std::floor(rect.y) == rect.y && std::floor(rect.width) == rect.width &&
           std::floor(rect.height) == rect.height && std::abs(rect.x) <= 8192 &&
           std::abs(rect.y) <= 8192 && rect.width > 0 && rect.width <= 4096 && rect.height > 0 &&
           rect.height <= 4096;
}

PopupSurfaceRequest Metadata(PopupSurfaceRequest request)
{
    request.source.reset();
    return request;
}

bool RequiresBackdrop(const SceneSnapshot &snapshot, contracts::NodeId id)
{
    const auto &node = snapshot.Get(id);
    if (!node.style.visible) {
        return false;
    }
    if (node.style.backdrop_blur > 0) {
        return true;
    }
    for (auto child : node.children) {
        if (RequiresBackdrop(snapshot, child)) {
            return true;
        }
    }
    return false;
}
} // namespace

SceneSnapshot Scene::CaptureResolvedSnapshot() const
{
    SceneSnapshot snapshot;
    if (theme_) {
        snapshot.controls = theme_->controls;
    }
    snapshot.root = root_->id;
    snapshot.nodes.resize(nodes_.size());
    for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
        snapshot.nodes[i].id = {static_cast<std::uint32_t>(i), 0};
        snapshot.nodes[i].style.visible = false;
    }
    for (const Node *node : nodes_) {
        if (!node) {
            continue;
        }
        SnapshotNode item;
        item.id = node->id;
        item.kind = node->kind;
        item.popup_anchor = node->popup_anchor;
        item.parent = node->parent ? node->parent->id : contracts::NodeId{};
        item.style = node->style;
        item.style.visible = IsVisible(*node);
        item.text = node->text;
        item.icon = node->icon;
        item.value = node->value;
        item.scroll_offset = node->scroll_offset;
        item.scroll_content_height = node->scroll_content_height;
        item.checked = node->checked;
        item.interaction = State(node->id);
        item.presentation = node->presentation;
        item.image = node->image;
        item.intrinsic_size = node->intrinsic_size;
        item.image_ready = node->image_ready;
        item.bounds = node->bounds;
        item.contour_source = node->contour_source;
        item.contour = node->contour;
        item.contour_spec = node->contour_spec;
        item.contour_request = node->contour_request;
        item.contour_prepared = node->contour_prepared;
        item.popup_placement = node->popup_placement;
        item.shaped = node->shaped;
        item.revision = node->revision;
        ApplyPresentation(*node, item);
        for (const auto &child : node->children) {
            item.children.push_back(child->id);
        }
        snapshot.nodes[node->id.index] = std::move(item);
    }
    ApplyPopupScrollOffsets(snapshot);
    return snapshot;
}

std::optional<PopupSurfaceRequest>
Scene::CapturePopupSurfaceRequest(std::uint64_t parent_configure_generation)
{
    return CapturePopupSurfaceRequest(
        parent_configure_generation,
        {0, 0, std::ceil(viewport_.width), std::ceil(viewport_.height)});
}

std::optional<PopupSurfaceRequest>
Scene::CapturePopupSurfaceRequest(std::uint64_t parent_configure_generation,
                                  contracts::LogicalRect parent_window_geometry)
{
    const auto *popup = Find(active_popup_);
    if (!popup || !IsVisible(*popup) || !IsEnabled(*popup) || !parent_configure_generation ||
        !IntegerRect(parent_window_geometry) || !popup_snapshot_ || !input_snapshot_ ||
        popup_snapshot_pixels_revision_ != pixels_revision_ || Has(dirty_, Dirty::Layout) ||
        Has(dirty_, Dirty::Paint)) {
        return std::nullopt;
    }

    if (popup_snapshot_revision_ != transaction_revision_) {
        auto snapshot = CaptureResolvedSnapshot();
        ApplyScrollVisuals(snapshot);
        ApplySliderVisuals(snapshot);
        auto prepared = std::make_shared<const SceneSnapshot>(std::move(snapshot));
        // Metadata-only packets do not call Build. Sample committed values,
        // including hidden theme styles, without layout or pixel invalidation.
        input_snapshot_dirty_ = true;
        UpdateInputSnapshot();
        popup_snapshot_ = std::move(prepared);
        popup_surface_source_.reset();
        popup_snapshot_revision_ = transaction_revision_;
    } else {
        UpdateInputSnapshot();
    }
    const auto &prepared = popup_snapshot_->Get(popup->id);
    if (!prepared.popup_placement || prepared.bounds.width <= 0 || prepared.bounds.height <= 0) {
        return std::nullopt;
    }

    PopupSurfaceRequest request;
    request.scene = input_scene_id_;
    request.popup_token = PopupToken();
    request.active_node = popup->id;
    request.trigger = popup->popup_anchor;
    request.parent_configure_generation = parent_configure_generation;
    request.scene_revision = transaction_revision_;
    request.pixels_revision = pixels_revision_;
    request.theme_generation = ThemeGeneration();
    request.requires_backdrop = RequiresBackdrop(*popup_snapshot_, popup->id);
    request.parent_window_geometry = parent_window_geometry;
    request.anchor = prepared.popup_placement->effective_anchor;
    request.desired_body = {prepared.style.width, prepared.style.height};
    const double neck =
        prepared.contour_spec ? contracts::PanelNeckHeight(*prepared.contour_spec) : 0;
    request.desired_geometry = {request.desired_body.width, request.desired_body.height + neck};
    request.horizontal_alignment = prepared.contour_spec
                                       ? contracts::PopupHorizontalAlignment::Center
                                       : contracts::PopupHorizontalAlignment::Start;
    request.vertical_preference = prepared.popup_placement->side == PopupSide::Above
                                      ? contracts::PopupVerticalPreference::Above
                                      : contracts::PopupVerticalPreference::Below;
    if (!contracts::PreparePopupPositioner({request.anchor, request.desired_geometry, request.gap,
                                            request.horizontal_alignment,
                                            request.vertical_preference},
                                           parent_window_geometry)) {
        return std::nullopt;
    }

    popup_parent_configure_generation_ = parent_configure_generation;
    if (popup_surface_source_ && popup_surface_source_->metadata == request &&
        popup_surface_source_->input == input_snapshot_) {
        request.source = popup_surface_source_;
        return request;
    }

    auto source = std::make_shared<PopupSurfaceSource>();
    source->metadata = request;
    source->snapshot = popup_snapshot_;
    source->input = input_snapshot_;
    source->window = popup_snapshot_window_;
    for (const auto *node : nodes_) {
        if (!node || !DescendantOf(node, *popup) ||
            (node->kind != Kind::Slider && node->kind != Kind::ScrollView)) {
            continue;
        }
        PopupControlLayout control;
        control.owner = node->id;
        if (node->kind == Kind::Slider) {
            const auto &domain = node->number_domain;
            control.slider_fraction = std::clamp((SliderPresentedValue(*node) - domain.minimum) /
                                                     (domain.maximum - domain.minimum),
                                                 0.0, 1.0);
        }
        for (const auto &child : node->children) {
            const auto &role = node->kind == Kind::Slider ? child->slider_part : child->scroll_part;
            if (!role.empty()) {
                control.parts.push_back({child->id, role, IsVisible(*child)});
            }
        }
        source->controls.push_back(std::move(control));
    }
    popup_surface_source_ = std::move(source);
    request.source = popup_surface_source_;
    return request;
}

std::optional<PopupSurfacePlan> Scene::PreparePopupSurface(const PopupSurfaceRequest &request,
                                                           const PopupSurfaceConfigure &configure,
                                                           std::string *diagnostic) const
{
    if (diagnostic) {
        diagnostic->clear();
    }
    try {
        const auto *popup = Find(active_popup_);
        if (!request.source || request.source != popup_surface_source_ ||
            request.source->snapshot != popup_snapshot_ ||
            request.source->input != input_snapshot_ ||
            request.source->metadata != Metadata(request) || !popup ||
            popup->id != request.active_node || popup->popup_anchor != request.trigger ||
            request.scene != input_scene_id_ || request.popup_token != PopupToken() ||
            request.scene_revision != transaction_revision_ ||
            request.pixels_revision != pixels_revision_ ||
            request.theme_generation != ThemeGeneration() || !IsVisible(*popup) ||
            !IsEnabled(*popup) || Has(dirty_, Dirty::Layout) || Has(dirty_, Dirty::Paint) ||
            request.parent_configure_generation != popup_parent_configure_generation_ ||
            configure.parent_configure_generation != request.parent_configure_generation ||
            !configure.configure_generation || !IntegerRect(configure.window_bounds)) {
            throw std::invalid_argument("Stale or invalid popup surface configure source");
        }

        auto plan = PreparePopupSurfaceValues(request, configure, shaper_, font_,
                                              popup_surface_prepared_.get());
        popup_surface_prepared_ = plan.prepared;
        ++popup_surface_preparation_stats_.preparations;
        if (plan.prepared->layout_reused) {
            ++popup_surface_preparation_stats_.layout_reuses;
        } else {
            ++popup_surface_preparation_stats_.layouts;
        }
        return plan;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &error) {
        if (diagnostic) {
            *diagnostic = error.what();
        }
        return std::nullopt;
    }
}

PopupSurfacePreparationStats Scene::GetPopupSurfacePreparationStats() const noexcept
{
    return popup_surface_preparation_stats_;
}
} // namespace prism::runtime
