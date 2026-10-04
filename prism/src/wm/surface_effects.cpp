#include "surface_effects_internal.hpp"

namespace prism::wm {
SurfaceEffects::Impl::Impl(wl_display *display, wlr_renderer *r, wlr_allocator *a)
    : renderer(r), allocator(a)
{
    supported = wlr_renderer_is_gles2(renderer);
    if (supported) {
        ContextScope context(renderer);
        if (context.current) {
            blur = Program(blur_shader);
            material = Program(material_shader);
        }
        supported = context.current && blur && material;
    }
    global = wl_global_create(display, &prism_surface_effect_manager_v1_interface, 1, this, Bind);
    PRISM_LOG_INFO("SURFACE-EFFECT", "GPU backdrop capability=%d (typed protocol v1)", supported);
}

SurfaceEffects::Impl::~Impl()
{
    wake_handler = {};
    while (!objects.empty()) {
        wl_resource_destroy(*objects.begin());
    }
    while (!managers.empty()) {
        wl_resource_destroy(*managers.begin());
    }
    for (auto &[surface, s] : states) {
        wl_list_remove(&s->commit.link);
        wl_list_remove(&s->destroy.link);
    }
    states.clear();
    paints.clear();
    if (global) {
        wl_global_destroy(global);
    }
    if (wlr_renderer_is_gles2(renderer)) {
        ContextScope context(renderer);
        if (context.current) {
            if (blur) {
                glDeleteProgram(blur);
            }
            if (material) {
                glDeleteProgram(material);
            }
        }
    }
}

SurfaceEffects::Impl::State *SurfaceEffects::Impl::Get(wl_resource *resource)
{
    return static_cast<State *>(wl_resource_get_user_data(resource));
}

void SurfaceEffects::Impl::Destroy(wl_client *, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

void SurfaceEffects::Impl::Clear(wl_client *, wl_resource *resource)
{
    if (auto *s = Get(resource)) {
        s->pending.clear();
        s->dirty = true;
    }
}

void SurfaceEffects::Impl::Add(wl_client *, wl_resource *resource, wl_fixed_t x, wl_fixed_t y,
                               wl_fixed_t w, wl_fixed_t h, wl_fixed_t radius,
                               wl_fixed_t blur_radius)
{
    auto *s = Get(resource);
    if (!s) {
        wl_resource_post_error(resource, 1, "Surface has been destroyed");
        return;
    }
    Region r{{wl_fixed_to_double(x), wl_fixed_to_double(y), wl_fixed_to_double(w),
              wl_fixed_to_double(h)},
             wl_fixed_to_double(radius),
             wl_fixed_to_double(blur_radius)};
    if (s->pending.size() >= 8 || std::abs(r.bounds.x) > 8192 || std::abs(r.bounds.y) > 8192 ||
        r.bounds.width <= 0 || r.bounds.height <= 0 || r.bounds.width > 8192 ||
        r.bounds.height > 8192 || r.corner_radius < 0 || r.corner_radius > 256 ||
        r.blur_radius < 0 || r.blur_radius > 48) {
        wl_resource_post_error(resource, 0, "Invalid surface effect region");
        return;
    }
    s->pending.push_back(r);
    s->dirty = true;
}

void SurfaceEffects::Impl::EffectGone(wl_resource *resource)
{
    auto *s = Get(resource);
    if (s) {
        s->owner->objects.erase(resource);
        s->resource = nullptr;
        s->pending.clear();
        s->dirty = true;
        s->owner->MarkDirty();
    }
}

void SurfaceEffects::Impl::Commit(wl_listener *listener, void *)
{
    auto *s =
        reinterpret_cast<State *>(reinterpret_cast<char *>(listener) - offsetof(State, commit));
    if (s->dirty) {
        s->current = s->pending;
        s->dirty = false;
        s->owner->MarkDirty();
    }
}

void SurfaceEffects::Impl::SurfaceGone(wl_listener *listener, void *)
{
    auto *s =
        reinterpret_cast<State *>(reinterpret_cast<char *>(listener) - offsetof(State, destroy));
    auto *owner = s->owner;
    auto *surface = s->surface;
    wl_list_remove(&s->commit.link);
    wl_list_remove(&s->destroy.link);
    if (s->resource) {
        owner->objects.erase(s->resource);
        wl_resource_set_user_data(s->resource, nullptr);
    }
    if (auto it = owner->paints.find(surface); it != owner->paints.end()) {
        owner->counters.removed_regions += it->second.size();
        owner->paints.erase(it);
    }
    owner->states.erase(surface);
    owner->MarkDirty();
}

void SurfaceEffects::Impl::NewEffect(wl_client *client, wl_resource *manager, uint32_t id,
                                     wl_resource *surface_resource)
{
    auto *owner = static_cast<Impl *>(wl_resource_get_user_data(manager));
    auto *surface = wlr_surface_from_resource(surface_resource);
    auto &state = owner->states[surface];
    if (!state) {
        state = std::make_unique<State>();
        state->owner = owner;
        state->surface = surface;
        state->commit.notify = Commit;
        state->destroy.notify = SurfaceGone;
        wl_signal_add(&surface->events.commit, &state->commit);
        wl_signal_add(&surface->events.destroy, &state->destroy);
    }
    if (state->resource) {
        wl_resource_post_error(manager, 0, "Surface already has an effect object");
        return;
    }
    auto *resource = wl_resource_create(client, &prism_surface_effect_v1_interface, 1, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct prism_surface_effect_v1_interface impl{Destroy, Clear, Add};
    state->resource = resource;
    owner->objects.insert(resource);
    wl_resource_set_implementation(resource, &impl, state.get(), EffectGone);
}

void SurfaceEffects::Impl::ManagerGone(wl_resource *resource)
{
    static_cast<Impl *>(wl_resource_get_user_data(resource))->managers.erase(resource);
}

void SurfaceEffects::Impl::Bind(wl_client *client, void *data, uint32_t version, uint32_t id)
{
    auto *owner = static_cast<Impl *>(data);
    auto *resource =
        wl_resource_create(client, &prism_surface_effect_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct prism_surface_effect_manager_v1_interface impl{Destroy, NewEffect};
    owner->managers.insert(resource);
    wl_resource_set_implementation(resource, &impl, owner, ManagerGone);
    prism_surface_effect_manager_v1_send_capabilities(resource, owner->supported);
}

SurfaceEffects::SurfaceEffects(wl_display *d, wlr_renderer *r, wlr_allocator *a)
    : impl_(std::make_unique<Impl>(d, r, a))
{
}

SurfaceEffects::~SurfaceEffects() = default;

bool SurfaceEffects::Supported() const
{
    return impl_->supported;
}

void SurfaceEffects::SetWakeHandler(std::function<void()> handler)
{
    impl_->wake_handler = std::move(handler);
    if (impl_->needs_update) {
        impl_->NotifyWake();
    }
}

void SurfaceEffects::MarkDirty()
{
    impl_->MarkDirty();
}

void SurfaceEffects::NotifySurfaceCommit(wlr_surface *surface)
{
    impl_->NotifySurfaceCommit(surface);
}

void SurfaceEffects::ForgetSurface(wlr_surface *surface)
{
    impl_->ForgetSurface(surface);
}

std::vector<wlr_scene_node *> SurfaceEffects::PresentationNodes(wlr_surface *surface) const
{
    std::vector<wlr_scene_node *> result;
    const auto found = impl_->paints.find(surface);
    if (found != impl_->paints.end()) {
        for (const auto &paint : found->second) {
            if (paint->tree && paint->valid) {
                result.push_back(&paint->tree->node);
            }
        }
    }
    return result;
}

bool SurfaceEffects::NeedsUpdate() const
{
    return impl_->needs_update;
}

const SurfaceEffects::WorkCounters &SurfaceEffects::Counters() const
{
    return impl_->counters;
}

SurfaceEffects::UpdateResult SurfaceEffects::Update(wlr_scene *s, std::span<WlrXdgView *const> v,
                                                    WlrXdgView *f,
                                                    const contracts::ThemeSnapshot *t)
{
    return impl_->Update(s, v, f, t);
}

} // namespace prism::wm
