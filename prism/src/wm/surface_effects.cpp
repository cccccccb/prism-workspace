#include "surface_effects_internal.hpp"

#include <new>
#include <span>
#include <stdexcept>

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
    global = wl_global_create(display, &prism_surface_effect_manager_v1_interface, 3, this, Bind);
    PRISM_LOG_INFO("SURFACE-EFFECT", "GPU backdrop capability=%d (typed protocol v3)", supported);
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
    auto *s = Get(resource);
    if (!s) {
        wl_resource_post_error(resource, PRISM_SURFACE_EFFECT_V1_ERROR_SURFACE_GONE,
                               "Surface has been destroyed");
        return;
    }

    s->pending.clear();
    s->dirty = true;
}

void SurfaceEffects::Impl::Add(wl_client *client, wl_resource *resource, wl_fixed_t x, wl_fixed_t y,
                               wl_fixed_t w, wl_fixed_t h, wl_fixed_t radius,
                               wl_fixed_t blur_radius)
{
    auto *s = Get(resource);
    if (!s) {
        wl_resource_post_error(resource, PRISM_SURFACE_EFFECT_V1_ERROR_SURFACE_GONE,
                               "Surface has been destroyed");
        return;
    }

    try {
        Region region{{wl_fixed_to_double(x), wl_fixed_to_double(y), wl_fixed_to_double(w),
                       wl_fixed_to_double(h)},
                      wl_fixed_to_double(radius),
                      wl_fixed_to_double(blur_radius)};
        contracts::ValidateSurfaceEffectRegion(region);
        if (s->pending.size() >= 8) {
            throw std::invalid_argument("Excessive surface effect regions");
        }

        s->pending.push_back(std::move(region));
        s->dirty = true;
    } catch (const std::bad_alloc &) {
        wl_client_post_no_memory(client);
    } catch (const std::exception &error) {
        wl_resource_post_error(resource, PRISM_SURFACE_EFFECT_V1_ERROR_INVALID_REGION, "%s",
                               error.what());
    } catch (...) {
        wl_resource_post_error(resource, PRISM_SURFACE_EFFECT_V1_ERROR_INVALID_REGION,
                               "Surface effect region rejected");
    }
}

void SurfaceEffects::Impl::AddContour(wl_client *client, wl_resource *resource,
                                      wl_fixed_t blur_radius, wl_array *payload)
{
    auto *s = Get(resource);
    if (!s) {
        wl_resource_post_error(resource, PRISM_SURFACE_EFFECT_V1_ERROR_SURFACE_GONE,
                               "Surface has been destroyed");
        return;
    }

    try {
        if (!s->owner->supported || s->pending.size() >= 8 || !payload || !payload->data ||
            payload->size < 32 || payload->size > 8 + contracts::ContourVertexLimit * 8) {
            throw std::invalid_argument("Invalid or unsupported surface effect contour");
        }
        const auto bytes =
            std::span(static_cast<const std::uint8_t *>(payload->data), payload->size);
        auto contour = contracts::DecodeContour(bytes);
        Region region{contracts::ContourBounds(contour), 0, wl_fixed_to_double(blur_radius),
                      std::move(contour)};
        contracts::ValidateSurfaceEffectRegion(region);

        s->pending.push_back(std::move(region));
        s->dirty = true;
    } catch (const std::bad_alloc &) {
        wl_client_post_no_memory(client);
    } catch (const std::exception &error) {
        wl_resource_post_error(resource, PRISM_SURFACE_EFFECT_V1_ERROR_INVALID_REGION, "%s",
                               error.what());
    } catch (...) {
        wl_resource_post_error(resource, PRISM_SURFACE_EFFECT_V1_ERROR_INVALID_REGION,
                               "Surface effect contour rejected");
    }
}

void SurfaceEffects::Impl::EffectGone(wl_resource *resource)
{
    auto *s = Get(resource);
    if (s) {
        s->owner->objects.erase(resource);
        s->resource = nullptr;
        s->pending.clear();
        s->dirty = true;
        try {
            s->owner->MarkDirty();
        } catch (...) {
            PRISM_LOG_ERROR("SURFACE-EFFECT", "effect destroy wake callback failed");
        }
    }
}

void SurfaceEffects::Impl::Commit(wl_listener *listener, void *)
{
    auto *s =
        reinterpret_cast<State *>(reinterpret_cast<char *>(listener) - offsetof(State, commit));
    if (s->dirty) {
        try {
            auto next = s->pending;
            s->current.swap(next);
            s->dirty = false;
            s->owner->MarkDirty();
        } catch (const std::bad_alloc &) {
            wl_resource_post_no_memory(s->surface->resource);
        } catch (const std::exception &error) {
            PRISM_LOG_ERROR("SURFACE-EFFECT", "commit callback: %s", error.what());
            wl_resource_post_error(s->surface->resource, 0, "Effect commit failed");
        } catch (...) {
            wl_resource_post_error(s->surface->resource, 0, "Effect commit failed");
        }
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
    try {
        owner->MarkDirty();
    } catch (...) {
        PRISM_LOG_ERROR("SURFACE-EFFECT", "surface destroy wake callback failed");
    }
}

void SurfaceEffects::Impl::NewEffect(wl_client *client, wl_resource *manager, uint32_t id,
                                     wl_resource *surface_resource)
{
    try {
        auto *owner = static_cast<Impl *>(wl_resource_get_user_data(manager));
        auto *surface = wlr_surface_from_resource(surface_resource);
        auto found = owner->states.find(surface);
        if (found == owner->states.end()) {
            auto prepared = std::make_unique<State>();
            prepared->owner = owner;
            prepared->surface = surface;
            prepared->commit.notify = Commit;
            prepared->destroy.notify = SurfaceGone;
            found = owner->states.emplace(surface, std::move(prepared)).first;
            wl_signal_add(&surface->events.commit, &found->second->commit);
            wl_signal_add(&surface->events.destroy, &found->second->destroy);
        }
        auto &state = found->second;
        if (state->resource) {
            wl_resource_post_error(manager, 0, "Surface already has an effect object");
            return;
        }

        const auto version = std::min(wl_resource_get_version(manager), 3);
        auto *resource =
            wl_resource_create(client, &prism_surface_effect_v1_interface, version, id);
        if (!resource) {
            wl_client_post_no_memory(client);
            return;
        }
        try {
            owner->objects.insert(resource);
        } catch (...) {
            wl_resource_destroy(resource);
            throw;
        }

        static const struct prism_surface_effect_v1_interface impl{Destroy, Clear, Add, AddContour};
        state->resource = resource;
        wl_resource_set_implementation(resource, &impl, state.get(), EffectGone);
    } catch (const std::bad_alloc &) {
        wl_client_post_no_memory(client);
    } catch (const std::exception &error) {
        wl_resource_post_error(manager, 0, "Cannot create effect: %s", error.what());
    } catch (...) {
        wl_resource_post_error(manager, 0, "Cannot create effect");
    }
}

void SurfaceEffects::Impl::ManagerGone(wl_resource *resource)
{
    static_cast<Impl *>(wl_resource_get_user_data(resource))->managers.erase(resource);
}

void SurfaceEffects::Impl::Bind(wl_client *client, void *data, uint32_t version, uint32_t id)
{
    auto *owner = static_cast<Impl *>(data);
    auto *resource = wl_resource_create(client, &prism_surface_effect_manager_v1_interface,
                                        std::min(version, 3u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct prism_surface_effect_manager_v1_interface impl{Destroy, NewEffect};
    try {
        owner->managers.insert(resource);
    } catch (const std::bad_alloc &) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    } catch (...) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &impl, owner, ManagerGone);
    prism_surface_effect_manager_v1_send_capabilities(resource, owner->supported);
    if (wl_resource_get_version(resource) >= 2) {
        prism_surface_effect_manager_v1_send_contour_capabilities(resource, owner->supported);
    }
    if (wl_resource_get_version(resource) >= 3) {
        prism_surface_effect_manager_v1_send_popup_backdrop_capabilities(resource,
                                                                         owner->supported);
    }
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
                                                    const contracts::ThemeSnapshot *t,
                                                    std::span<const Target> popups)
{
    return impl_->Update(s, v, f, t, popups);
}

} // namespace prism::wm
