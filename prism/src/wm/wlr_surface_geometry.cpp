#include "wlr_surface_geometry.hpp"
#include "wlr_server_internal.hpp"

namespace prism::wm {
namespace {
contracts::LogicalRect Source(WlrXdgView *view)
{
    wlr_box box{};
    wlr_xdg_surface_get_geometry(view->toplevel->base, &box);
    return {double(box.x), double(box.y), double(std::max(1, box.width)),
            double(std::max(1, box.height))};
}

contracts::ThemeDecoration VisibleStyle(contracts::ThemeDecoration style)
{
    if (!style.enabled) {
        style.radius = style.border_width = style.shadow_blur = style.shadow_y = 0;
        style.border.a = style.shadow.a = 0;
    }
    return style;
}

contracts::Color Mix(contracts::Color a, contracts::Color b, double t)
{
    return {std::uint8_t(std::lround(std::lerp(a.r, b.r, t))),
            std::uint8_t(std::lround(std::lerp(a.g, b.g, t))),
            std::uint8_t(std::lround(std::lerp(a.b, b.b, t))),
            std::uint8_t(std::lround(std::lerp(a.a, b.a, t)))};
}

contracts::ThemeDecoration Mix(contracts::ThemeDecoration a, contracts::ThemeDecoration b, double t)
{
    if (t >= 1) {
        return b;
    }
    a = VisibleStyle(a);
    b = VisibleStyle(b);
    return {a.enabled || b.enabled,
            std::lerp(a.radius, b.radius, t),
            std::lerp(a.border_width, b.border_width, t),
            Mix(a.border, b.border, t),
            std::lerp(a.shadow_blur, b.shadow_blur, t),
            std::lerp(a.shadow_y, b.shadow_y, t),
            Mix(a.shadow, b.shadow, t)};
}

struct SurfaceOffset {
    wlr_surface *wanted;
    double x{}, y{};
    bool found{};

    static void Visit(wlr_surface *surface, int x, int y, void *data)
    {
        auto &self = *static_cast<SurfaceOffset *>(data);
        if (surface == self.wanted) {
            self.x = x;
            self.y = y;
            self.found = true;
        }
    }
};
} // namespace

struct SurfaceGeometry::SavedNode {
    wl_listener destroy{};
    wlr_scene_node *node;
    int x, y, width{}, height{}, destination_width{}, destination_height{};
    pixman_region32_t opaque;

    explicit SavedNode(wlr_scene_node *n) : node(n), x(n->x), y(n->y)
    {
        pixman_region32_init(&opaque);
        if (n->type == WLR_SCENE_NODE_BUFFER) {
            auto *buffer = wlr_scene_buffer_from_node(n);
            destination_width = buffer->dst_width;
            destination_height = buffer->dst_height;
            auto *surface = wlr_scene_surface_try_from_buffer(buffer);
            width = destination_width ? destination_width
                                      : (surface ? surface->surface->current.width : 0);
            height = destination_height ? destination_height
                                        : (surface ? surface->surface->current.height : 0);
            pixman_region32_copy(&opaque, &buffer->opaque_region);
        } else if (n->type == WLR_SCENE_NODE_RECT) {
            auto *rect = wlr_scene_rect_from_node(n);
            width = rect->width;
            height = rect->height;
        }
        destroy.notify = Destroy;
        wl_signal_add(&node->events.destroy, &destroy);
    }

    ~SavedNode()
    {
        wl_list_remove(&destroy.link);
        pixman_region32_fini(&opaque);
    }

    static void Destroy(wl_listener *listener, void *)
    {
        auto *self = reinterpret_cast<SavedNode *>(listener);
        self->node = nullptr;
        wl_list_remove(&listener->link);
        wl_list_init(&listener->link);
    }

    void Restore()
    {
        if (!node) {
            return;
        }
        wlr_scene_node_set_position(node, x, y);
        if (node->type == WLR_SCENE_NODE_BUFFER) {
            auto *buffer = wlr_scene_buffer_from_node(node);
            wlr_scene_buffer_set_dest_size(buffer, destination_width, destination_height);
            wlr_scene_buffer_set_opaque_region(buffer, &opaque);
        } else if (node->type == WLR_SCENE_NODE_RECT) {
            wlr_scene_rect_set_size(wlr_scene_rect_from_node(node), width, height);
        }
    }
};

SurfaceGeometry::SurfaceGeometry(WlrXdgView *view)
    : geometry_(clock_), decoration_(clock_), view_(view)
{
}

SurfaceGeometry::~SurfaceGeometry()
{
    Restore();
    view_->presentation.reset();
}

WlrXdgView *SurfaceGeometry::View() const noexcept
{
    return view_;
}

contracts::LogicalRect SurfaceGeometry::Submitted() const noexcept
{
    return submitted_.bounds;
}

contracts::ThemeDecoration SurfaceGeometry::Decoration() const noexcept
{
    return submitted_style_;
}

void SurfaceGeometry::Start(contracts::LogicalRect from, contracts::LogicalRect target,
                            contracts::ThemeDecoration from_style,
                            contracts::ThemeDecoration target_style,
                            const contracts::MotionTransition &spec)
{
    Restore();
    const auto now = clock_.NowNs();
    const animation::DurationSpec duration{std::uint64_t(spec.duration_ms) * 1'000'000, 0,
                                           static_cast<animation::Easing>(spec.easing)};
    geometry_.Reset(from);
    geometry_.Retarget(target, duration, now);
    decoration_.Cancel();
    decoration_.StartDurationAt(0, 1, duration, now);
    from_style_ = submitted_style_ = from_style;
    target_style_ = target_style;
    if (submitted_.bounds.width <= 0) {
        submitted_source_ = Source(view_);
    }
    submitted_ = {from, 0, animation::MotionState::Running};
    retry_ = false;
    dirty_ = true;
}

void SurfaceGeometry::Restore()
{
    dirty_ |= !saved_.empty();
    for (auto &node : saved_) {
        node->Restore();
    }
    saved_.clear();
}

bool SurfaceGeometry::Capture(wlr_scene_node *node)
{
    if (saved_.size() >= 512) {
        return false;
    }
    saved_.push_back(std::make_unique<SavedNode>(node));
    if (node->type == WLR_SCENE_NODE_BUFFER && node->enabled &&
        (saved_.back()->width <= 0 || saved_.back()->height <= 0)) {
        return false;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        wlr_scene_node *child;
        wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link)
        {
            if (!Capture(child)) {
                return false;
            }
        }
    }
    return true;
}

void SurfaceGeometry::Transform()
{
    const auto &bounds = candidate_.bounds;
    pixman_region32_t transparent;
    pixman_region32_init(&transparent);
    const double scale_x = bounds.width / source_.width, scale_y = bounds.height / source_.height;
    for (const auto &saved : saved_) {
        auto *node = saved->node;
        const bool root = node == &view_->scene_tree->node;
        wlr_scene_node_set_position(node,
                                    root ? int(bounds.x) : int(std::lround(saved->x * scale_x)),
                                    root ? int(bounds.y) : int(std::lround(saved->y * scale_y)));
        if (node->type == WLR_SCENE_NODE_BUFFER) {
            auto *buffer = wlr_scene_buffer_from_node(node);
            wlr_scene_buffer_set_dest_size(buffer,
                                           std::max(1, int(std::lround(saved->width * scale_x))),
                                           std::max(1, int(std::lround(saved->height * scale_y))));
            wlr_scene_buffer_set_opaque_region(buffer, &transparent);
        } else if (node->type == WLR_SCENE_NODE_RECT) {
            wlr_scene_rect_set_size(wlr_scene_rect_from_node(node),
                                    std::max(1, int(std::lround(saved->width * scale_x))),
                                    std::max(1, int(std::lround(saved->height * scale_y))));
        }
    }
    pixman_region32_fini(&transparent);
}

bool SurfaceGeometry::Prepare()
{
    Restore();
    if (!ValidTarget()) {
        return false;
    }
    const auto now = clock_.NowNs();
    candidate_ = geometry_.Sample(now);
    auto &r = candidate_.bounds;
    r = {std::round(r.x), std::round(r.y), std::max(1.0, std::round(r.width)),
         std::max(1.0, std::round(r.height))};
    source_ = Source(view_);
    const auto style = Mix(from_style_, target_style_, decoration_.SampleAt(now).value);
    if (!Capture(&view_->scene_tree->node)) {
        Restore();
        return false;
    }
    Transform();
    view_->presentation = NativePresentation{r, source_, style};
    dirty_ = false;
    return true;
}

bool SurfaceGeometry::SubmittedFrame(bool success)
{
    retry_ = !success;
    if (!success) {
        return false;
    }
    submitted_ = candidate_;
    submitted_source_ = source_;
    submitted_style_ = view_->presentation->decoration;
    return !geometry_.Running() && source_.width == view_->width && source_.height == view_->height;
}

bool SurfaceGeometry::NeedsFrame() const noexcept
{
    return geometry_.Running() || retry_ || dirty_;
}

bool SurfaceGeometry::ValidTarget() const noexcept
{
    return view_->mapped && view_->visible &&
           geometry_.Target() == contracts::LogicalRect{double(view_->x), double(view_->y),
                                                        double(view_->width),
                                                        double(view_->height)};
}

wlr_surface *SurfaceGeometry::Hit(double x, double y, double &sx, double &sy) const
{
    const auto &r = submitted_.bounds;
    if (x < r.x || y < r.y || x >= r.x + r.width || y >= r.y + r.height) {
        return nullptr;
    }
    const auto local =
        animation::GeometryTimeline::ToSurface(submitted_, submitted_source_, {x, y});
    return wlr_surface_surface_at(view_->toplevel->base->surface, local.x, local.y, &sx, &sy);
}

bool SurfaceGeometry::Position(wlr_surface *surface, double x, double y, double &sx,
                               double &sy) const
{
    SurfaceOffset offset{surface};
    wlr_surface_for_each_surface(view_->toplevel->base->surface, SurfaceOffset::Visit, &offset);
    if (!offset.found) {
        return false;
    }
    const auto local =
        animation::GeometryTimeline::ToSurface(submitted_, submitted_source_, {x, y});
    sx = local.x - offset.x;
    sy = local.y - offset.y;
    return true;
}
} // namespace prism::wm
