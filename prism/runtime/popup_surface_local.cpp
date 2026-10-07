#include "popup_surface_p.hpp"

#include "prism/contracts/rounded_region.hpp"
#include "prism/runtime/scene_snapshot.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace prism::runtime {
namespace {
using Rect = contracts::LogicalRect;

double PadX(const Style &style)
{
    return style.padding_x < 0 ? style.padding : style.padding_x;
}

double PadY(const Style &style)
{
    return style.padding_y < 0 ? style.padding : style.padding_y;
}

Rect SliderTrack(const SceneSnapshot &snapshot, const PopupControlLayout &control)
{
    const auto &owner = snapshot.Get(control.owner);
    const double px = PadX(owner.style);
    const double py = PadY(owner.style);
    const double width = std::max(0.0, owner.bounds.width - 2 * px);
    const double height = std::max(0.0, owner.bounds.height - 2 * py);
    double thumb_width = 0;
    for (const auto &part : control.parts) {
        if (part.role == "thumb") {
            thumb_width = std::min(width, snapshot.Get(part.id).style.width);
        }
    }
    return {owner.bounds.x + px + thumb_width / 2, owner.bounds.y + py + height / 2,
            std::max(0.0, width - thumb_width), height};
}

void ApplySliderVisuals(SceneSnapshot &snapshot, const PopupControlLayout &control)
{
    if (!std::isfinite(control.slider_fraction) || control.slider_fraction < 0 ||
        control.slider_fraction > 1) {
        throw std::invalid_argument("Invalid immutable Popup Slider fraction");
    }

    const auto &owner = snapshot.Get(control.owner);
    const auto track = SliderTrack(snapshot, control);
    const double available_width = std::max(0.0, owner.bounds.width - 2 * PadX(owner.style));
    for (const auto &source : control.parts) {
        auto &part = snapshot.Get(source.id);
        const double height = std::min(track.height, part.style.height);
        const double width = std::min(available_width, part.style.width);
        if (source.role == "thumb") {
            part.bounds = {track.x + track.width * control.slider_fraction - width / 2,
                           track.y - height / 2, width, height};
        } else {
            part.bounds = {track.x, track.y - height / 2,
                           source.role == "fill" ? track.width * control.slider_fraction
                                                 : track.width,
                           height};
        }
    }
}

void ApplyScrollVisuals(SceneSnapshot &snapshot, const PopupControlLayout &control)
{
    const auto &owner = snapshot.Get(control.owner);
    const double maximum = std::max(0.0, owner.scroll_content_height - owner.bounds.height);
    for (const auto &source : control.parts) {
        auto &part = snapshot.Get(source.id);
        part.style.visible = source.visible && maximum > 0;
        const double inset = part.style.inset;
        const double track = std::max(0.0, owner.bounds.height - 2 * inset);
        double height = track;
        double y = owner.bounds.y + inset;
        if (source.role == "thumb" && maximum > 0) {
            height = std::min(track, std::max(part.style.height, track * owner.bounds.height /
                                                                     owner.scroll_content_height));
            y += (track - height) * owner.scroll_offset / maximum;
        }
        const double width = std::min(part.style.width, owner.bounds.width);
        part.bounds = {owner.bounds.x + std::max(0.0, owner.bounds.width - inset - width), y, width,
                       height};
    }
}

const PopupControlLayout *FindControl(const PopupSurfaceSource &source, contracts::NodeId owner)
{
    for (const auto &control : source.controls) {
        if (control.owner == owner) {
            return &control;
        }
    }
    return nullptr;
}

void CopyInputNode(const SceneSnapshot &snapshot, const PopupSurfaceSource &source,
                   InputSnapshot &result, contracts::NodeId id, bool parent_visible,
                   bool parent_enabled)
{
    const auto *original = source.input->Find(id);
    if (!original) {
        throw std::logic_error("Popup local input requires immutable source node metadata");
    }

    const auto &node = snapshot.Get(id);
    auto &item = result.nodes[id.index];
    item = *original;
    item.parent = id == result.root ? contracts::NodeId{} : node.parent;
    item.bounds = node.bounds;
    item.radius = node.contour ? 0 : node.style.radius;
    item.contour = node.contour;
    item.scroll_offset = node.scroll_offset;
    item.visible = parent_visible && original->visible && node.style.visible;
    item.enabled = parent_enabled && original->enabled && node.interaction.enabled;
    if (node.kind == Kind::Slider) {
        const auto *control = FindControl(source, id);
        if (!control) {
            throw std::logic_error("Popup Slider input requires immutable control metadata");
        }
        item.slider_track = SliderTrack(snapshot, *control);
    }

    for (const auto child : item.children) {
        CopyInputNode(snapshot, source, result, child, item.visible, item.enabled);
    }
}

void ValidateViewport(contracts::LogicalSize viewport)
{
    if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height) || viewport.width <= 0 ||
        viewport.height <= 0) {
        throw std::invalid_argument("Popup local input requires a finite positive viewport");
    }
}
} // namespace

void ApplyPopupControlVisuals(SceneSnapshot &snapshot, const PopupSurfaceSource &source)
{
    for (const auto &control : source.controls) {
        const auto &owner = snapshot.Get(control.owner);
        if (owner.kind == Kind::Slider) {
            ApplySliderVisuals(snapshot, control);
        } else if (owner.kind == Kind::ScrollView) {
            ApplyScrollVisuals(snapshot, control);
        }
    }
}

std::shared_ptr<const InputSnapshot> PreparePopupInput(const SceneSnapshot &snapshot,
                                                       const PopupSurfaceSource &source,
                                                       contracts::LogicalSize viewport)
{
    ValidateViewport(viewport);
    if (!source.input || !source.input->Find(snapshot.root)) {
        throw std::logic_error("Popup local input requires an immutable source snapshot");
    }

    InputSnapshot result;
    // This descriptor is deliberately inadmissible to the current root Scene
    // input API. Native target-scoped submission is a separate protocol step.
    result.scene = 0;
    result.popup_token = source.input->popup_token;
    result.owner_modal_epoch = source.input->owner_modal_epoch;
    result.version = source.input->version;
    result.root = snapshot.root;
    result.viewport = viewport;
    result.nodes.resize(snapshot.nodes.size());
    CopyInputNode(snapshot, source, result, result.root, true, true);
    return std::make_shared<const InputSnapshot>(std::move(result));
}

std::vector<contracts::SurfaceInputRegion> PreparePopupInputRegions(const SceneSnapshot &snapshot,
                                                                    contracts::NodeId root,
                                                                    contracts::LogicalSize viewport)
{
    ValidateViewport(viewport);
    const auto &node = snapshot.Get(root);
    if (!IsPopupKind(node.kind)) {
        throw std::logic_error("Popup local input region requires a Popup or Menu root");
    }
    if (!node.style.visible || node.bounds.width <= 0 || node.bounds.height <= 0) {
        return {};
    }
    if ((node.contour_source || node.contour_spec) && !node.contour) {
        throw std::logic_error("Popup input regions require a prepared contour");
    }

    const Rect clip{0, 0, viewport.width, viewport.height};
    std::vector<Rect> mask;
    if (node.contour) {
        const std::array contours{*node.contour};
        mask = contracts::RasterizeContourIntersection(contours, clip);
    } else {
        const std::array rounded{
            contracts::NormalizeRoundedRegion({node.bounds, node.style.radius})};
        mask = contracts::RasterizeContourIntersection({}, clip, rounded);
    }

    // Popup roots are material input clips. Their exact region already covers
    // every descendant; shadow padding remains outside this union.
    if (mask.size() > 65536) {
        throw std::length_error("Popup input region limit is 65536 rectangles");
    }
    std::vector<contracts::SurfaceInputRegion> result;
    result.reserve(mask.size());
    for (const auto &bounds : mask) {
        result.push_back({bounds, 0});
    }
    return result;
}
} // namespace prism::runtime
