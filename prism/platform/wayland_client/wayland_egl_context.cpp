#include "prism/platform/wayland_egl_context.hpp"
#include "prism/platform/wayland_egl_surface.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <new>
#include <string_view>

namespace prism::platform {
namespace {
std::uint64_t NextLifetimeIdentity() noexcept
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load();
    while (value) {
        const auto following = value == std::numeric_limits<std::uint64_t>::max() ? 0 : value + 1;
        if (next.compare_exchange_weak(value, following)) {
            return value;
        }
    }
    return 0;
}

bool HasExtension(std::string_view extensions, std::string_view extension)
{
    std::size_t offset = 0;
    while (offset < extensions.size()) {
        while (offset < extensions.size() &&
               std::isspace(static_cast<unsigned char>(extensions[offset]))) {
            ++offset;
        }
        const auto begin = offset;
        while (offset < extensions.size() &&
               !std::isspace(static_cast<unsigned char>(extensions[offset]))) {
            ++offset;
        }
        if (extensions.substr(begin, offset - begin) == extension) {
            return true;
        }
    }
    return false;
}
} // namespace

WaylandEglContext::~WaylandEglContext()
{
    Close();
}

bool WaylandEglContext::Open(wl_display *display)
{
    if (!display || native_display_ || closing_) {
        return false;
    }
    const auto identity = NextLifetimeIdentity();
    if (!identity) {
        return false;
    }

    native_display_ = display;
    egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display));
    if (egl_display_ == EGL_NO_DISPLAY || !eglInitialize(egl_display_, nullptr, nullptr)) {
        Close();
        return false;
    }
    initialized_ = true;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        Close();
        return false;
    }
    const EGLint attributes[] = {EGL_SURFACE_TYPE,
                                 EGL_WINDOW_BIT,
                                 EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES3_BIT,
                                 EGL_RED_SIZE,
                                 8,
                                 EGL_GREEN_SIZE,
                                 8,
                                 EGL_BLUE_SIZE,
                                 8,
                                 EGL_ALPHA_SIZE,
                                 8,
                                 EGL_NONE};
    EGLint count{};
    if (!eglChooseConfig(egl_display_, attributes, &egl_config_, 1, &count) || count != 1) {
        Close();
        return false;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, context_attributes);
    if (egl_context_ == EGL_NO_CONTEXT) {
        Close();
        return false;
    }
    lifetime_id_ = identity;
    DiscoverCapabilities();
    return true;
}

bool WaylandEglContext::Ready() const noexcept
{
    return native_display_ && egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT &&
           lifetime_id_;
}

void WaylandEglContext::DiscoverCapabilities()
{
    const auto *extension_string = eglQueryString(egl_display_, EGL_EXTENSIONS);
    const std::string_view extensions = extension_string ? extension_string : "";
    ext_buffer_age_ = HasExtension(extensions, "EGL_EXT_buffer_age");
    partial_update_ = HasExtension(extensions, "EGL_KHR_partial_update");
    if (HasExtension(extensions, "EGL_KHR_swap_buffers_with_damage")) {
        swap_damage_ = reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(
            eglGetProcAddress("eglSwapBuffersWithDamageKHR"));
    }
    if (!swap_damage_ && HasExtension(extensions, "EGL_EXT_swap_buffers_with_damage")) {
        swap_damage_ = reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(
            eglGetProcAddress("eglSwapBuffersWithDamageEXT"));
    }
    if (partial_update_) {
        set_damage_region_ = reinterpret_cast<PFNEGLSETDAMAGEREGIONKHRPROC>(
            eglGetProcAddress("eglSetDamageRegionKHR"));
    }
}

bool WaylandEglContext::RegisterSurface(WaylandEglSurface &surface)
{
    if (!Ready() || closing_) {
        return false;
    }
    const auto identity = NextLifetimeIdentity();
    if (!identity) {
        return false;
    }
    try {
        surfaces_.push_back(&surface);
    } catch (const std::bad_alloc &) {
        return false;
    }
    surface.target_identity_ = {lifetime_id_, identity, 1};
    return true;
}

void WaylandEglContext::DetachSurface(WaylandEglSurface &surface) noexcept
{
    std::erase(surfaces_, &surface);
    const bool current = surface.egl_surface_ != EGL_NO_SURFACE &&
                         eglGetCurrentDisplay() == egl_display_ &&
                         eglGetCurrentContext() == egl_context_ &&
                         (eglGetCurrentSurface(EGL_DRAW) == surface.egl_surface_ ||
                          eglGetCurrentSurface(EGL_READ) == surface.egl_surface_);
    if (!current || closing_) {
        return;
    }
    for (auto *other : surfaces_) {
        if (other->MakeCurrent()) {
            return;
        }
    }
    eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void WaylandEglContext::Close() noexcept
{
    if (closing_) {
        return;
    }
    closing_ = true;
    if (egl_display_ != EGL_NO_DISPLAY && eglGetCurrentDisplay() == egl_display_ &&
        egl_context_ != EGL_NO_CONTEXT && eglGetCurrentContext() == egl_context_) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    while (!surfaces_.empty()) {
        surfaces_.back()->Close();
    }
    if (egl_context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display_, egl_context_);
    }
    if (initialized_) {
        eglTerminate(egl_display_);
    }

    native_display_ = nullptr;
    egl_display_ = EGL_NO_DISPLAY;
    egl_config_ = nullptr;
    egl_context_ = EGL_NO_CONTEXT;
    lifetime_id_ = 0;
    swap_damage_ = nullptr;
    set_damage_region_ = nullptr;
    ext_buffer_age_ = partial_update_ = false;
    initialized_ = false;
    closing_ = false;
}
} // namespace prism::platform
