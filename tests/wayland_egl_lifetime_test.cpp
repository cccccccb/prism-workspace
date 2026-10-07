#include "prism/contracts/gpu_target.hpp"
#include "prism/platform/wayland_egl_context.hpp"
#include "prism/platform/wayland_egl_surface.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <wayland-client-core.h>
#include <wayland-egl.h>

// Native symbols are supplied by this executable. The production context and
// WSI classes remain the implementation under test; no compositor or driver is
// needed to observe binding, submission, and destruction at their boundary.
struct wl_display {
    int id{};
};

struct wl_surface {
    wl_display *display{};
};

struct wl_egl_window {
    wl_surface *surface{};
    int width{};
    int height{};
    bool alive{true};
};

namespace {
enum class Failure {
    None,
    GetDisplay,
    Initialize,
    BindApi,
    ChooseConfig,
    CreateContext,
    CreateWindow,
    CreateSurface,
    MakeCurrent
};

enum class EventKind {
    MakeCurrent,
    BindFramebuffer,
    Unbind,
    DestroySurface,
    DestroyWindow,
    DestroyContext,
    Terminate,
    SetDamage,
    Swap,
    SwapDamage
};

struct NativeEvent {
    EventKind kind{};
    EGLSurface surface{EGL_NO_SURFACE};
};

struct NativeSurface {
    wl_egl_window *window{};
    bool alive{};
    int age{1};
    EGLint swap_behavior{EGL_BUFFER_DESTROYED};
    bool fail_age{};
    bool fail_damage{};
    bool fail_swap{};
    bool fail_current{};
    std::size_t age_queries{};
    std::size_t damage_calls{};
    std::size_t swaps{};
    std::size_t damage_swaps{};
    std::vector<EGLint> repair;
    std::vector<EGLint> content;
};

struct NativeCounts {
    std::size_t display_queries{};
    std::size_t initializes{};
    std::size_t context_creates{};
    std::size_t context_destroys{};
    std::size_t terminates{};
    std::size_t window_creates{};
    std::size_t window_destroys{};
    std::size_t window_resizes{};
    std::size_t surface_creates{};
    std::size_t surface_destroys{};
    std::size_t unbinds{};
    std::size_t framebuffer_binds{};
};

class NativeApi;
NativeApi *native_api{};

class NativeApi {
public:
    NativeApi()
    {
        assert(!native_api);
        native_api = this;
    }

    ~NativeApi()
    {
        CheckReleased();
        native_api = nullptr;
    }

    bool Fails(Failure call)
    {
        if (failure != call) {
            return false;
        }
        failure = Failure::None;
        return true;
    }

    EGLDisplay Display()
    {
        return reinterpret_cast<EGLDisplay>(&display_token);
    }

    EGLContext Context()
    {
        return reinterpret_cast<EGLContext>(&context_token);
    }

    EGLConfig Config()
    {
        return reinterpret_cast<EGLConfig>(&config_token);
    }

    NativeSurface &Surface(EGLSurface handle)
    {
        const auto found =
            std::find_if(surfaces.begin(), surfaces.end(), [handle](const auto &item) {
                return reinterpret_cast<EGLSurface>(item.get()) == handle;
            });
        assert(found != surfaces.end());
        assert((*found)->alive && (*found)->window && (*found)->window->alive);
        return **found;
    }

    EGLSurface CreateSurface(wl_egl_window *window)
    {
        NativeSurface *surface = nullptr;
        if (recycle_surface) {
            for (const auto &candidate : surfaces) {
                if (!candidate->alive) {
                    surface = candidate.get();
                    break;
                }
            }
        }
        if (!surface) {
            surfaces.push_back(std::make_unique<NativeSurface>());
            surface = surfaces.back().get();
        }
        *surface = {};
        surface->alive = true;
        surface->window = window;
        surface->swap_behavior = next_swap_behavior;
        next_swap_behavior = EGL_BUFFER_DESTROYED;
        ++counts.surface_creates;
        return reinterpret_cast<EGLSurface>(surface);
    }

    void CheckReleased() const
    {
        assert(!initialized && !context_alive);
        assert(current_display == EGL_NO_DISPLAY && current_context == EGL_NO_CONTEXT);
        assert(current_draw == EGL_NO_SURFACE && current_read == EGL_NO_SURFACE);
        for (const auto &surface : surfaces) {
            assert(!surface->alive);
        }
        for (const auto &window : windows) {
            assert(!window->alive);
        }
        assert(counts.context_creates == counts.context_destroys);
        assert(counts.surface_creates == counts.surface_destroys);
        assert(counts.window_creates == counts.window_destroys);
    }

    wl_display connection{1};
    wl_display other_connection{2};
    wl_surface parent{&connection};
    wl_surface child{&connection};
    wl_surface foreign{&other_connection};
    Failure failure{Failure::None};
    bool recycle_surface{};
    bool initialized{};
    bool context_alive{};
    EGLint next_swap_behavior{EGL_BUFFER_DESTROYED};
    std::string extensions{"EGL_EXT_buffer_age EGL_KHR_partial_update "
                           "EGL_KHR_swap_buffers_with_damage"};
    EGLDisplay current_display{EGL_NO_DISPLAY};
    EGLContext current_context{EGL_NO_CONTEXT};
    EGLSurface current_draw{EGL_NO_SURFACE};
    EGLSurface current_read{EGL_NO_SURFACE};
    GLuint framebuffer{};
    NativeCounts counts;
    std::vector<NativeEvent> events;
    std::vector<std::unique_ptr<wl_egl_window>> windows;
    std::vector<std::unique_ptr<NativeSurface>> surfaces;

private:
    int display_token{};
    int context_token{};
    int config_token{};
};

NativeApi &Api()
{
    assert(native_api);
    return *native_api;
}

void CheckDisplay(EGLDisplay display)
{
    assert(display == Api().Display() && Api().initialized);
}

void RecordRectangles(std::vector<EGLint> &destination, const EGLint *rectangles, EGLint count)
{
    assert(count >= 0 && (rectangles || count == 0));
    destination.clear();
    if (count) {
        destination.assign(rectangles, rectangles + count * 4);
    }
}

EGLBoolean Post(EGLDisplay display, EGLSurface handle, const EGLint *rectangles, EGLint count,
                bool with_damage)
{
    CheckDisplay(display);
    auto &surface = Api().Surface(handle);
    assert(Api().current_display == display && Api().current_context == Api().Context());
    assert(Api().current_draw == handle && Api().current_read == handle);
    if (with_damage) {
        ++surface.damage_swaps;
        RecordRectangles(surface.content, rectangles, count);
    } else {
        ++surface.swaps;
        surface.content.clear();
    }
    Api().events.push_back({with_damage ? EventKind::SwapDamage : EventKind::Swap, handle});
    if (std::exchange(surface.fail_swap, false)) {
        return EGL_FALSE;
    }
    return EGL_TRUE;
}
} // namespace

extern "C" {
EGLAPI EGLDisplay EGLAPIENTRY eglGetDisplay(EGLNativeDisplayType display_id)
{
    auto &api = Api();
    ++api.counts.display_queries;
    assert(display_id == reinterpret_cast<EGLNativeDisplayType>(&api.connection));
    return api.Fails(Failure::GetDisplay) ? EGL_NO_DISPLAY : api.Display();
}

EGLAPI EGLBoolean EGLAPIENTRY eglInitialize(EGLDisplay display, EGLint *major, EGLint *minor)
{
    auto &api = Api();
    assert(display == api.Display());
    ++api.counts.initializes;
    if (api.Fails(Failure::Initialize)) {
        return EGL_FALSE;
    }
    assert(!api.initialized);
    api.initialized = true;
    if (major) {
        *major = 1;
    }
    if (minor) {
        *minor = 5;
    }
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglBindAPI(EGLenum api)
{
    assert(api == EGL_OPENGL_ES_API);
    return Api().Fails(Failure::BindApi) ? EGL_FALSE : EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglChooseConfig(EGLDisplay display, const EGLint *,
                                              EGLConfig *configs, EGLint config_size,
                                              EGLint *num_config)
{
    CheckDisplay(display);
    assert(configs && config_size >= 1 && num_config);
    if (Api().Fails(Failure::ChooseConfig)) {
        *num_config = 0;
        return EGL_FALSE;
    }
    *configs = Api().Config();
    *num_config = 1;
    return EGL_TRUE;
}

EGLAPI EGLContext EGLAPIENTRY eglCreateContext(EGLDisplay display, EGLConfig config,
                                               EGLContext share_context, const EGLint *)
{
    CheckDisplay(display);
    auto &api = Api();
    assert(config == api.Config() && share_context == EGL_NO_CONTEXT && !api.context_alive);
    if (api.Fails(Failure::CreateContext)) {
        return EGL_NO_CONTEXT;
    }
    ++api.counts.context_creates;
    api.context_alive = true;
    return api.Context();
}

EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
                                                     EGLNativeWindowType window, const EGLint *)
{
    CheckDisplay(display);
    auto &api = Api();
    assert(config == api.Config() && api.context_alive);
    auto *native_window = reinterpret_cast<wl_egl_window *>(window);
    assert(native_window && native_window->alive);
    return api.Fails(Failure::CreateSurface) ? EGL_NO_SURFACE : api.CreateSurface(native_window);
}

EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read,
                                             EGLContext context)
{
    auto &api = Api();
    assert(display == api.Display());
    if (context == EGL_NO_CONTEXT) {
        assert(draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE);
        api.current_display = EGL_NO_DISPLAY;
        api.current_context = EGL_NO_CONTEXT;
        api.current_draw = api.current_read = EGL_NO_SURFACE;
        ++api.counts.unbinds;
        api.events.push_back({EventKind::Unbind});
        return EGL_TRUE;
    }
    CheckDisplay(display);
    assert(context == api.Context() && api.context_alive && draw == read);
    auto &surface = api.Surface(draw);
    if (api.Fails(Failure::MakeCurrent) || std::exchange(surface.fail_current, false)) {
        return EGL_FALSE;
    }
    api.current_display = display;
    api.current_context = context;
    api.current_draw = draw;
    api.current_read = read;
    api.events.push_back({EventKind::MakeCurrent, draw});
    return EGL_TRUE;
}

EGLAPI EGLDisplay EGLAPIENTRY eglGetCurrentDisplay()
{
    return Api().current_display;
}

EGLAPI EGLContext EGLAPIENTRY eglGetCurrentContext()
{
    return Api().current_context;
}

EGLAPI EGLSurface EGLAPIENTRY eglGetCurrentSurface(EGLint readdraw)
{
    assert(readdraw == EGL_DRAW || readdraw == EGL_READ);
    return readdraw == EGL_DRAW ? Api().current_draw : Api().current_read;
}

EGLAPI const char *EGLAPIENTRY eglQueryString(EGLDisplay display, EGLint name)
{
    CheckDisplay(display);
    assert(name == EGL_EXTENSIONS);
    return Api().extensions.c_str();
}

EGLAPI EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay display, EGLSurface handle,
                                              EGLint attribute, EGLint *value)
{
    CheckDisplay(display);
    assert(value);
    auto &surface = Api().Surface(handle);
    switch (attribute) {
    case EGL_WIDTH:
        *value = surface.window->width;
        break;
    case EGL_HEIGHT:
        *value = surface.window->height;
        break;
    case EGL_SWAP_BEHAVIOR:
        *value = surface.swap_behavior;
        break;
    case EGL_BUFFER_AGE_EXT:
        ++surface.age_queries;
        if (std::exchange(surface.fail_age, false)) {
            return EGL_FALSE;
        }
        *value = surface.age;
        break;
    default:
        assert(false);
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSetDamageRegionKHR(EGLDisplay display, EGLSurface handle,
                                                    EGLint *rectangles, EGLint count)
{
    CheckDisplay(display);
    auto &surface = Api().Surface(handle);
    assert(Api().current_draw == handle && Api().current_context == Api().Context());
    ++surface.damage_calls;
    RecordRectangles(surface.repair, rectangles, count);
    Api().events.push_back({EventKind::SetDamage, handle});
    return std::exchange(surface.fail_damage, false) ? EGL_FALSE : EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapBuffersWithDamageKHR(EGLDisplay display, EGLSurface handle,
                                                          const EGLint *rectangles, EGLint count)
{
    return Post(display, handle, rectangles, count, true);
}

EGLAPI EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay display, EGLSurface handle)
{
    return Post(display, handle, nullptr, 0, false);
}

EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char *name)
{
    assert(name);
    if (std::strcmp(name, "eglSetDamageRegionKHR") == 0) {
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglSetDamageRegionKHR);
    }
    if (std::strcmp(name, "eglSwapBuffersWithDamageKHR") == 0 ||
        std::strcmp(name, "eglSwapBuffersWithDamageEXT") == 0) {
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(
            eglSwapBuffersWithDamageKHR);
    }
    return nullptr;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay display, EGLSurface handle)
{
    CheckDisplay(display);
    auto &api = Api();
    auto &surface = api.Surface(handle);
    assert(api.current_draw != handle && api.current_read != handle);
    surface.alive = false;
    ++api.counts.surface_destroys;
    api.events.push_back({EventKind::DestroySurface, handle});
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglDestroyContext(EGLDisplay display, EGLContext context)
{
    CheckDisplay(display);
    auto &api = Api();
    assert(context == api.Context() && api.context_alive);
    assert(api.current_context != context);
    for (const auto &surface : api.surfaces) {
        assert(!surface->alive);
    }
    api.context_alive = false;
    ++api.counts.context_destroys;
    api.events.push_back({EventKind::DestroyContext});
    return EGL_TRUE;
}

EGLAPI EGLBoolean EGLAPIENTRY eglTerminate(EGLDisplay display)
{
    auto &api = Api();
    assert(display == api.Display() && !api.context_alive);
    for (const auto &surface : api.surfaces) {
        assert(!surface->alive);
    }
    for (const auto &window : api.windows) {
        assert(!window->alive);
    }
    assert(api.current_display != display);
    api.initialized = false;
    ++api.counts.terminates;
    api.events.push_back({EventKind::Terminate});
    return EGL_TRUE;
}

GL_APICALL void GL_APIENTRY glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    auto &api = Api();
    assert(target == GL_FRAMEBUFFER && framebuffer == 0);
    assert(api.current_display == api.Display() && api.current_context == api.Context());
    assert(api.current_draw != EGL_NO_SURFACE && api.current_read == api.current_draw);
    api.Surface(api.current_draw);
    api.framebuffer = framebuffer;
    ++api.counts.framebuffer_binds;
    api.events.push_back({EventKind::BindFramebuffer, api.current_draw});
}

GL_APICALL const GLubyte *GL_APIENTRY glGetString(GLenum name)
{
    assert(Api().current_context == Api().Context());
    const char *value = nullptr;
    switch (name) {
    case GL_VENDOR:
        value = "test vendor";
        break;
    case GL_RENDERER:
        value = "test renderer";
        break;
    case GL_VERSION:
        value = "OpenGL ES 3.0 test";
        break;
    default:
        assert(false);
    }
    return reinterpret_cast<const GLubyte *>(value);
}

wl_display *wl_proxy_get_display(wl_proxy *proxy)
{
    auto &api = Api();
    auto *surface = reinterpret_cast<wl_surface *>(proxy);
    assert(surface == &api.parent || surface == &api.child || surface == &api.foreign);
    return surface->display;
}

wl_egl_window *wl_egl_window_create(wl_surface *surface, int width, int height)
{
    auto &api = Api();
    assert(surface && surface->display == &api.connection && width > 0 && height > 0);
    if (api.Fails(Failure::CreateWindow)) {
        return nullptr;
    }
    auto window = std::make_unique<wl_egl_window>();
    window->surface = surface;
    window->width = width;
    window->height = height;
    auto *result = window.get();
    api.windows.push_back(std::move(window));
    ++api.counts.window_creates;
    return result;
}

void wl_egl_window_destroy(wl_egl_window *window)
{
    auto &api = Api();
    assert(window && window->alive);
    for (const auto &surface : api.surfaces) {
        assert(!surface->alive || surface->window != window);
    }
    window->alive = false;
    ++api.counts.window_destroys;
    api.events.push_back({EventKind::DestroyWindow});
}

void wl_egl_window_resize(wl_egl_window *window, int width, int height, int dx, int dy)
{
    assert(window && window->alive && width > 0 && height > 0 && dx == 0 && dy == 0);
    window->width = width;
    window->height = height;
    ++Api().counts.window_resizes;
}

void wl_egl_window_get_attached_size(wl_egl_window *window, int *width, int *height)
{
    assert(window && window->alive && width && height);
    *width = window->width;
    *height = window->height;
}
} // extern "C"

namespace {
using prism::contracts::DamageRegion;
using prism::contracts::GpuTargetIdentity;
using prism::platform::DamageRegionResult;
using prism::platform::WaylandEglContext;
using prism::platform::WaylandEglSurface;

EGLSurface CurrentSurface(WaylandEglSurface &surface)
{
    assert(surface.MakeCurrent());
    const auto handle = eglGetCurrentSurface(EGL_DRAW);
    assert(handle != EGL_NO_SURFACE && handle == eglGetCurrentSurface(EGL_READ));
    assert(eglGetCurrentContext() == Api().Context());
    return handle;
}

void CheckCurrent(EGLSurface handle)
{
    assert(eglGetCurrentDisplay() == Api().Display());
    assert(eglGetCurrentContext() == Api().Context());
    assert(eglGetCurrentSurface(EGL_DRAW) == handle);
    assert(eglGetCurrentSurface(EGL_READ) == handle);
    assert(Api().framebuffer == 0);
}

void CheckIdentity(const GpuTargetIdentity &target, std::uint64_t generation = 1)
{
    assert(target);
    assert(target.context_lifetime_id && target.surface_lifetime_id);
    assert(target.resize_generation == generation);
}

std::size_t Posts(const NativeSurface &surface)
{
    return surface.swaps + surface.damage_swaps;
}

DamageRegion Delta(int x = 3, int y = 7, int width = 9, int height = 11)
{
    return {false, {{x, y, width, height}}};
}

void CheckSharedContextAndSwitching()
{
    NativeApi api;
    WaylandEglContext context;
    WaylandEglSurface parent;
    WaylandEglSurface child;
    assert(context.Open(&api.connection) && context.Ready());
    assert(!context.Open(&api.connection));
    assert(parent.Open(context, &api.parent, 200, 120));
    const auto parent_handle = CurrentSurface(parent);
    const auto parent_target = parent.TargetIdentity();
    CheckIdentity(parent_target);
    assert(child.Open(context, &api.child, 80, 60));
    const auto child_handle = CurrentSurface(child);
    const auto child_target = child.TargetIdentity();
    CheckIdentity(child_target);
    assert(parent_handle != child_handle);
    assert(parent_target.context_lifetime_id == child_target.context_lifetime_id);
    assert(parent_target.surface_lifetime_id != child_target.surface_lifetime_id);
    assert(api.counts.initializes == 1 && api.counts.context_creates == 1);
    assert(!child.Open(context, &api.child, 80, 60));

    assert(parent.MakeCurrent());
    CheckCurrent(parent_handle);
    assert(child.MakeCurrent());
    CheckCurrent(child_handle);
    assert(parent.MakeCurrent());
    const auto unbinds = api.counts.unbinds;
    child.Close();
    assert(!child.Ready() && !child.TargetIdentity());
    CheckCurrent(parent_handle);
    assert(api.counts.unbinds == unbinds);
    assert(api.counts.context_destroys == 0 && api.counts.terminates == 0);
    const auto events = api.events.size();
    child.Close();
    assert(api.events.size() == events);

    assert(child.Open(context, &api.child, 80, 60));
    CurrentSurface(child);
    child.Close();
    CheckCurrent(parent_handle);
    assert(parent.Ready() && context.Ready());
    parent.Close();
    assert(!parent.Ready() && !parent.TargetIdentity());
    assert(eglGetCurrentContext() == EGL_NO_CONTEXT);
    assert(context.Ready() && api.counts.context_destroys == 0);
    context.Close();
    assert(!context.Ready());
    assert(api.counts.context_destroys == 1 && api.counts.terminates == 1);
    const auto closed_events = api.events.size();
    context.Close();
    assert(api.events.size() == closed_events);
}

void CheckPlatformOwnsDefaultFramebuffer()
{
    NativeApi api;
    WaylandEglContext context;
    WaylandEglSurface parent;
    WaylandEglSurface child;
    assert(context.Open(&api.connection));
    assert(parent.Open(context, &api.parent, 100, 80));
    const auto parent_handle = CurrentSurface(parent);
    assert(child.Open(context, &api.child, 60, 40));
    const auto child_handle = CurrentSurface(child);

    api.framebuffer = 41;
    const auto before_parent = api.counts.framebuffer_binds;
    assert(parent.MakeCurrent());
    CheckCurrent(parent_handle);
    assert(api.counts.framebuffer_binds == before_parent + 1);
    assert(api.events.back().kind == EventKind::BindFramebuffer &&
           api.events.back().surface == parent_handle);
    api.framebuffer = 53;
    assert(child.MakeCurrent());
    CheckCurrent(child_handle);
    assert(api.events.back().kind == EventKind::BindFramebuffer &&
           api.events.back().surface == child_handle);

    assert(parent.MakeCurrent());
    api.framebuffer = 67;
    api.Surface(child_handle).fail_current = true;
    const auto before_failure = api.counts.framebuffer_binds;
    assert(!child.MakeCurrent());
    assert(api.framebuffer == 67 && api.counts.framebuffer_binds == before_failure);
    assert(eglGetCurrentSurface(EGL_DRAW) == parent_handle);
    assert(parent.MakeCurrent());
    CheckCurrent(parent_handle);
}

void CheckOwnerClosesExternalSurfaces()
{
    NativeApi api;
    WaylandEglSurface parent;
    WaylandEglSurface child;
    auto context = std::make_unique<WaylandEglContext>();
    assert(context->Open(&api.connection));
    assert(parent.Open(*context, &api.parent, 120, 80));
    assert(child.Open(*context, &api.child, 60, 40));
    const auto old_parent = parent.TargetIdentity();
    const auto old_child = child.TargetIdentity();
    const auto begin = api.events.size();
    context->Close();
    assert(!context->Ready() && !parent.Ready() && !child.Ready());
    assert(!parent.TargetIdentity() && !child.TargetIdentity());
    assert(!parent.MakeCurrent() && !child.MakeCurrent());
    assert(!parent.Resize(120, 80) && !child.QueryBufferAge());
    assert(parent.SetDamage(DamageRegion::Full()) == DamageRegionResult::Failed);
    assert(!parent.Swap() && !child.Swap());
    assert(api.counts.surface_destroys == 2 && api.counts.window_destroys == 2);
    assert(api.counts.context_destroys == 1 && api.counts.terminates == 1);
    const auto destroy =
        std::find_if(api.events.begin() + begin, api.events.end(), [](const NativeEvent &event) {
            return event.kind == EventKind::DestroyContext;
        });
    assert(destroy != api.events.end());
    assert(std::none_of(destroy, api.events.end(), [](const NativeEvent &event) {
        return event.kind == EventKind::DestroySurface || event.kind == EventKind::DestroyWindow;
    }));
    assert(api.events.back().kind == EventKind::Terminate);
    const auto events = api.events.size();
    parent.Close();
    child.Close();
    assert(api.events.size() == events);

    assert(context->Open(&api.connection));
    assert(parent.Open(*context, &api.parent, 120, 80));
    assert(child.Open(*context, &api.child, 60, 40));
    assert(parent.TargetIdentity().context_lifetime_id != old_parent.context_lifetime_id);
    assert(parent.TargetIdentity().surface_lifetime_id != old_parent.surface_lifetime_id);
    assert(child.TargetIdentity().surface_lifetime_id != old_child.surface_lifetime_id);
    context.reset();
    assert(!parent.Ready() && !child.Ready());
    assert(!parent.TargetIdentity() && !child.TargetIdentity());
    parent.Close();
    child.Close();
    api.CheckReleased();
}

void CheckIdentityWithRecycledNativeHandles()
{
    NativeApi api;
    api.recycle_surface = true;
    WaylandEglContext context;
    WaylandEglSurface surface;
    assert(context.Open(&api.connection));
    assert(surface.Open(context, &api.parent, 100, 80));
    const auto handle = CurrentSurface(surface);
    const auto first = surface.TargetIdentity();
    CheckIdentity(first);
    assert(surface.Resize(100, 80));
    assert(surface.TargetIdentity() == first && api.counts.window_resizes == 0);
    assert(surface.Resize(120, 90));
    const auto resized = surface.TargetIdentity();
    CheckIdentity(resized, 2);
    assert(resized.context_lifetime_id == first.context_lifetime_id);
    assert(resized.surface_lifetime_id == first.surface_lifetime_id);
    assert(CurrentSurface(surface) == handle && api.counts.window_resizes == 1);
    assert(!surface.Resize(0, 90) && !surface.Resize(120, 4097));
    assert(surface.TargetIdentity() == resized);
    surface.Close();
    assert(!surface.TargetIdentity());

    assert(surface.Open(context, &api.parent, 120, 90));
    const auto reopened = surface.TargetIdentity();
    CheckIdentity(reopened);
    assert(CurrentSurface(surface) == handle);
    assert(reopened.context_lifetime_id == first.context_lifetime_id);
    assert(reopened.surface_lifetime_id != first.surface_lifetime_id);
    context.Close();
    assert(!surface.Ready());
    assert(context.Open(&api.connection));
    assert(surface.Open(context, &api.parent, 120, 90));
    const auto new_context = surface.TargetIdentity();
    CheckIdentity(new_context);
    assert(CurrentSurface(surface) == handle);
    assert(new_context.context_lifetime_id != reopened.context_lifetime_id);
    assert(new_context.surface_lifetime_id != reopened.surface_lifetime_id);
}

void CheckOpenValidationAndChildFailures()
{
    NativeApi api;
    WaylandEglContext context;
    WaylandEglSurface parent;
    WaylandEglSurface child;
    assert(!context.Open(nullptr));
    assert(!child.Open(context, &api.child, 60, 40));
    assert(api.counts.display_queries == 0 && api.counts.window_creates == 0);
    assert(context.Open(&api.connection));
    assert(!child.Open(context, nullptr, 60, 40));
    assert(!child.Open(context, &api.child, 0, 40));
    assert(!child.Open(context, &api.child, 60, -1));
    assert(!child.Open(context, &api.child, 4097, 40));
    assert(!child.Open(context, &api.child, 60, 4097));
    assert(!child.Open(context, &api.foreign, 60, 40));
    assert(api.counts.window_creates == 0 && api.counts.surface_creates == 0);
    assert(parent.Open(context, &api.parent, 100, 80));
    const auto parent_handle = CurrentSurface(parent);
    const auto parent_target = parent.TargetIdentity();

    constexpr std::array failures{Failure::CreateWindow, Failure::CreateSurface,
                                  Failure::MakeCurrent};
    for (const auto failure : failures) {
        api.failure = failure;
        assert(!child.Open(context, &api.child, 60, 40));
        assert(api.failure == Failure::None);
        assert(!child.Ready() && !child.TargetIdentity());
        assert(parent.Ready() && context.Ready());
        assert(parent.TargetIdentity() == parent_target);
        CheckCurrent(parent_handle);
        assert(api.counts.context_creates == 1 && api.counts.context_destroys == 0);
        assert(api.counts.terminates == 0);
        child.Close();
        assert(parent.MakeCurrent());
    }
    assert(child.Open(context, &api.child, 60, 40));
    CheckIdentity(child.TargetIdentity());
}

void CheckContextInitializationFailures()
{
    constexpr std::array failures{Failure::GetDisplay, Failure::Initialize, Failure::BindApi,
                                  Failure::ChooseConfig, Failure::CreateContext};
    for (const auto failure : failures) {
        NativeApi api;
        WaylandEglContext context;
        WaylandEglSurface surface;
        api.failure = failure;
        assert(!context.Open(&api.connection));
        assert(api.failure == Failure::None && !context.Ready());
        assert(!surface.Open(context, &api.parent, 100, 80));
        if (failure == Failure::GetDisplay || failure == Failure::Initialize) {
            assert(api.counts.terminates == 0);
        }
        context.Close();
        api.CheckReleased();

        assert(context.Open(&api.connection));
        assert(surface.Open(context, &api.parent, 100, 80));
        assert(surface.MakeCurrent());
        context.Close();
        api.CheckReleased();
    }
}

void CheckIndependentAgeDamageAndResize()
{
    NativeApi api;
    WaylandEglContext context;
    WaylandEglSurface parent;
    WaylandEglSurface child;
    assert(context.Open(&api.connection));
    assert(parent.Open(context, &api.parent, 100, 80));
    const auto parent_handle = CurrentSurface(parent);
    assert(child.Open(context, &api.child, 60, 40));
    const auto child_handle = CurrentSurface(child);
    auto &parent_native = api.Surface(parent_handle);
    auto &child_native = api.Surface(child_handle);
    parent_native.age = 2;
    child_native.age = 4;
    assert(parent.Capabilities().buffer_age && parent.Capabilities().partial_update);
    assert(child.Capabilities().buffer_age && child.Capabilities().partial_update);
    assert(parent.QueryBufferAge() == 0 && child.QueryBufferAge() == 0);
    assert(parent.SetDamage(DamageRegion::Full()) == DamageRegionResult::Applied);
    assert(child.SetDamage(DamageRegion::Full()) == DamageRegionResult::Applied);
    assert(parent.Swap(Delta()) && child.Swap(Delta()));
    assert(parent_native.swaps == 1 && child_native.swaps == 1);
    assert(parent_native.damage_swaps == 0 && child_native.damage_swaps == 0);

    assert(parent.QueryBufferAge() == 2 && child.QueryBufferAge() == 4);
    const auto parent_queries = parent_native.age_queries;
    const auto child_queries = child_native.age_queries;
    parent_native.age = 3;
    child_native.age = 6;
    assert(parent.QueryBufferAge() == 2 && child.QueryBufferAge() == 4);
    assert(parent_native.age_queries == parent_queries &&
           child_native.age_queries == child_queries);
    assert(parent.SetDamage(Delta()) == DamageRegionResult::Applied);
    assert(child.SetDamage(Delta()) == DamageRegionResult::Applied);
    assert((parent_native.repair == std::vector<EGLint>{3, 62, 9, 11}));
    assert((child_native.repair == std::vector<EGLint>{3, 22, 9, 11}));
    assert(parent.Swap(Delta(1, 2, 5, 6)));
    assert(child.Swap(Delta(2, 3, 4, 5)));
    assert((parent_native.content == std::vector<EGLint>{1, 72, 5, 6}));
    assert((child_native.content == std::vector<EGLint>{2, 32, 4, 5}));
    assert(parent_native.damage_swaps == 1 && child_native.damage_swaps == 1);
    assert(parent.QueryBufferAge() == 3 && child.QueryBufferAge() == 6);

    const auto parent_target = parent.TargetIdentity();
    const auto child_target = child.TargetIdentity();
    assert(child.Resize(70, 50));
    assert(parent.TargetIdentity() == parent_target);
    assert(child.TargetIdentity().context_lifetime_id == child_target.context_lifetime_id);
    assert(child.TargetIdentity().surface_lifetime_id == child_target.surface_lifetime_id);
    CheckIdentity(child.TargetIdentity(), 2);
    child_native.age = 8;
    assert(child.QueryBufferAge() == 0 && parent.QueryBufferAge() == 3);
    assert(child.SetDamage(DamageRegion::Full()) == DamageRegionResult::Applied);
    assert(parent.SetDamage(Delta()) == DamageRegionResult::Applied);
    const auto declared_target = child.TargetIdentity();
    assert(child.Resize(70, 50));
    assert(!child.Resize(75, 50));
    assert(child.TargetIdentity() == declared_target);
    assert(child.Swap(Delta()) && parent.Swap(Delta()));
    assert(child_native.swaps == 2 && child_native.damage_swaps == 1);
    assert(parent_native.swaps == 1 && parent_native.damage_swaps == 2);
    assert(child.QueryBufferAge() == 8 && parent.QueryBufferAge() == 3);
}

void CheckChildFailureIsolation()
{
    NativeApi api;
    WaylandEglContext context;
    WaylandEglSurface parent;
    WaylandEglSurface child;
    assert(context.Open(&api.connection));
    assert(parent.Open(context, &api.parent, 100, 80));
    const auto parent_handle = CurrentSurface(parent);
    assert(child.Open(context, &api.child, 60, 40));
    auto child_handle = CurrentSurface(child);
    auto &parent_native = api.Surface(parent_handle);
    parent_native.age = 2;
    api.Surface(child_handle).age = 3;
    assert(parent.Swap() && child.Swap());
    auto &failed_damage = api.Surface(child_handle);
    failed_damage.fail_damage = true;
    const auto damage_calls = failed_damage.damage_calls;
    const auto posts = Posts(failed_damage);
    assert(child.SetDamage(Delta()) == DamageRegionResult::Failed);
    assert(child.SetDamage(DamageRegion::Full()) == DamageRegionResult::Failed);
    assert(failed_damage.damage_calls == damage_calls + 1);
    assert(!child.Swap() && !child.QueryBufferAge());
    assert(!child.Resize(70, 50) && Posts(failed_damage) == posts);
    assert(parent.SetDamage(Delta()) == DamageRegionResult::Applied);
    assert(parent.Swap(Delta()) && parent.QueryBufferAge() == 2);
    child.Close();
    CheckCurrent(parent_handle);

    assert(child.Open(context, &api.child, 60, 40));
    child_handle = CurrentSurface(child);
    auto &failed_swap = api.Surface(child_handle);
    failed_swap.age = 3;
    assert(child.Swap());
    failed_swap.fail_age = true;
    assert(!child.QueryBufferAge());
    assert(parent.QueryBufferAge() == 2 && child.QueryBufferAge() == 3);
    failed_swap.fail_swap = true;
    const auto previous_posts = Posts(failed_swap);
    assert(!child.Swap(Delta()));
    assert(Posts(failed_swap) == previous_posts + 1);
    assert(!child.Swap() && !child.Swap(Delta()));
    assert(Posts(failed_swap) == previous_posts + 1);
    assert(!child.QueryBufferAge());
    assert(parent.SetDamage(Delta()) == DamageRegionResult::Applied);
    assert(parent.Swap(Delta()) && parent.QueryBufferAge() == 2);
}

void CheckPerSurfaceDamageCapabilities()
{
    NativeApi api;
    WaylandEglContext context;
    WaylandEglSurface parent;
    WaylandEglSurface child;
    assert(context.Open(&api.connection));
    assert(parent.Open(context, &api.parent, 100, 80));
    const auto parent_handle = CurrentSurface(parent);
    api.next_swap_behavior = EGL_BUFFER_PRESERVED;
    assert(child.Open(context, &api.child, 60, 40));
    const auto child_handle = CurrentSurface(child);
    assert(parent.Capabilities().partial_update);
    assert(!child.Capabilities().partial_update && child.Capabilities().buffer_age);
    assert(child.SetDamage(DamageRegion::Full()) == DamageRegionResult::Unsupported);
    assert(api.Surface(child_handle).damage_calls == 0);
    assert(child.Swap());
    api.Surface(child_handle).age = 3;
    assert(child.QueryBufferAge() == 3);
    assert(child.SetDamage(Delta()) == DamageRegionResult::Unsupported);
    assert(child.Swap(Delta()));
    assert(parent.QueryBufferAge() == 0);
    assert(parent.SetDamage(DamageRegion::Full()) == DamageRegionResult::Applied);
    assert(parent.Swap());
    assert(api.Surface(parent_handle).damage_calls == 1);
}

void CheckFailedParentRebindDuringChildClose()
{
    NativeApi api;
    WaylandEglContext context;
    WaylandEglSurface parent;
    WaylandEglSurface child;
    assert(context.Open(&api.connection));
    assert(parent.Open(context, &api.parent, 100, 80));
    const auto parent_handle = CurrentSurface(parent);
    assert(child.Open(context, &api.child, 60, 40));
    const auto child_handle = CurrentSurface(child);
    api.Surface(parent_handle).fail_current = true;
    child.Close();
    assert(!child.Ready() && parent.Ready() && context.Ready());
    assert(eglGetCurrentSurface(EGL_DRAW) != child_handle);
    assert(api.counts.context_destroys == 0 && api.counts.terminates == 0);
    assert(parent.MakeCurrent());
    CheckCurrent(parent_handle);
}
} // namespace

int main()
{
    CheckSharedContextAndSwitching();
    CheckPlatformOwnsDefaultFramebuffer();
    CheckOwnerClosesExternalSurfaces();
    CheckIdentityWithRecycledNativeHandles();
    CheckOpenValidationAndChildFailures();
    CheckContextInitializationFailures();
    CheckIndependentAgeDamageAndResize();
    CheckChildFailureIsolation();
    CheckPerSurfaceDamageCapabilities();
    CheckFailedParentRebindDuringChildClose();
}
