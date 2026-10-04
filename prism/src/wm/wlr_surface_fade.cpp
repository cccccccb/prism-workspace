#include "wlr_surface_fade.hpp"
#include "wlr_server_internal.hpp"
#include <utility>

namespace prism::wm {
namespace {
bool RejectInput(wlr_scene_buffer *, double *, double *)
{
    return false;
}

void Opacity(wlr_scene_node *node, float value)
{
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        wlr_scene_buffer_set_opacity(wlr_scene_buffer_from_node(node), value);
    } else if (node->type == WLR_SCENE_NODE_TREE) {
        auto *tree = wlr_scene_tree_from_node(node);
        wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link)
        {
            Opacity(child, value);
        }
    }
}

bool Clone(wlr_scene_node *node, wlr_scene_tree *parent, int x, int y, unsigned &count)
{
    if (!node->enabled) {
        return true;
    }

    x += node->x;
    y += node->y;
    if (node->type == WLR_SCENE_NODE_TREE) {
        auto *tree = wlr_scene_tree_from_node(node);
        wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link)
        {
            if (!Clone(child, parent, x, y, count)) {
                return false;
            }
        }
        return true;
    }
    if (node->type != WLR_SCENE_NODE_BUFFER || ++count > 256) {
        return false;
    }

    auto *source = wlr_scene_buffer_from_node(node);
    auto *buffer = source->buffer;
    if (!buffer) {
        auto *surface = wlr_scene_surface_try_from_buffer(source);
        if (surface && surface->surface->buffer) {
            buffer = &surface->surface->buffer->base;
        }
    }
    if (!buffer) {
        return false;
    }

    auto *copy = wlr_scene_buffer_create(parent, buffer);
    if (!copy) {
        return false;
    }

    copy->point_accepts_input = RejectInput;
    wlr_scene_node_set_position(&copy->node, x, y);
    wlr_scene_buffer_set_source_box(copy, &source->src_box);
    wlr_scene_buffer_set_dest_size(copy, source->dst_width, source->dst_height);
    wlr_scene_buffer_set_transform(copy, source->transform);
    wlr_scene_buffer_set_filter_mode(copy, source->filter_mode);
    // No opaque region: an exit snapshot must never occlude live lower content.
    return true;
}
} // namespace

SurfaceFade::SurfaceFade() : timeline_(clock_)
{
}

SurfaceFade::~SurfaceFade()
{
    ClearSnapshot();
}

animation::DurationSpec SurfaceFade::Duration() const noexcept
{
    return {std::uint64_t(spec_.duration_ms) * 1'000'000, 0,
            static_cast<animation::Easing>(spec_.easing)};
}

void SurfaceFade::ClearSnapshot()
{
    if (snapshot_) {
        wlr_scene_node_destroy(&snapshot_->node);
        snapshot_ = nullptr;
    }
}

void SurfaceFade::ApplyLive(SurfaceEffects *effects, float opacity)
{
    if (!live_) {
        return;
    }
    Opacity(&live_->scene_tree->node, opacity);
    if (effects) {
        for (auto *node : effects->PresentationNodes(live_->toplevel->base->surface)) {
            Opacity(node, opacity);
        }
    }
}

void SurfaceFade::Open(WlrXdgView *view, SurfaceEffects *effects, contracts::MotionTransition spec)
{
    const auto current =
        snapshot_ && x_ == view->x && y_ == view->y ? float(timeline_.Sample().value) : 0.0f;
    Reset(effects);

    live_ = view;
    x_ = view->x;
    y_ = view->y;
    spec_ = std::move(spec);
    opacity_ = spec_.duration_ms ? current : 1;
    pending_ = spec_.duration_ms != 0;
    ApplyLive(effects, opacity_);
}

void SurfaceFade::Close(WlrXdgView *view, SurfaceEffects *effects)
{
    if (live_ != view || !spec_.duration_ms || pending_) {
        Reset(effects);
        return;
    }

    const auto current = timeline_.State() == animation::MotionState::Running
                             ? float(timeline_.Sample().value)
                             : opacity_;
    ApplyLive(effects, 1);

    ClearSnapshot();
    snapshot_ = wlr_scene_tree_create(view->scene_tree->node.parent);
    unsigned count{};
    bool copied = snapshot_ != nullptr;
    if (copied && effects) {
        for (auto *node : effects->PresentationNodes(view->toplevel->base->surface)) {
            copied = copied && Clone(node, snapshot_, 0, 0, count);
        }
    }
    copied = copied && Clone(&view->scene_tree->node, snapshot_, 0, 0, count) && count;
    live_ = nullptr;
    pending_ = false;
    if (!copied) {
        ClearSnapshot();
        timeline_.Cancel();
        return;
    }

    timeline_.RestartDuration(current, 0, Duration());
    Opacity(&snapshot_->node, current);
}

void SurfaceFade::Reset(SurfaceEffects *effects)
{
    ApplyLive(effects, 1);
    live_ = nullptr;
    pending_ = false;
    timeline_.Cancel();
    ClearSnapshot();
    opacity_ = 1;
}

bool SurfaceFade::Advance(SurfaceEffects *effects)
{
    if (pending_) {
        auto *surface = live_->toplevel->base->surface;
        if (!surface->buffer || surface->current.width != live_->width ||
            surface->current.height != live_->height) {
            ApplyLive(effects, opacity_);
            return false; // The client's next buffer commit wakes us; no busy frames.
        }
        pending_ = false;
        timeline_.StartDuration(opacity_, 1, Duration());
    }

    if (timeline_.State() == animation::MotionState::Running) {
        const auto sample = timeline_.Sample();
        opacity_ = float(sample.value);
        if (live_) {
            ApplyLive(effects, opacity_);
        } else if (snapshot_) {
            Opacity(&snapshot_->node, opacity_);
        }
        if (sample.state == animation::MotionState::Finished) {
            ClearSnapshot();
        }
    } else if (live_) {
        ApplyLive(effects, opacity_);
    }
    return Active();
}

bool SurfaceFade::Active() const noexcept
{
    return timeline_.State() == animation::MotionState::Running;
}
} // namespace prism::wm
