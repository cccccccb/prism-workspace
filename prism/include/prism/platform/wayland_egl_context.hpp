#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdint>
#include <vector>

struct wl_display;

namespace prism::platform {
class WaylandEglSurface;

// One GLES context for a Wayland connection. Context, WSI and GPU resources
// stay on the same render owner thread. Release GPU wrappers before Close;
// Close detaches registered WSI before destroying the context and display.
class WaylandEglContext {
public:
    WaylandEglContext() = default;
    ~WaylandEglContext();
    WaylandEglContext(const WaylandEglContext &) = delete;
    WaylandEglContext &operator=(const WaylandEglContext &) = delete;
    WaylandEglContext(WaylandEglContext &&) = delete;
    WaylandEglContext &operator=(WaylandEglContext &&) = delete;

    bool Open(wl_display *);
    bool Ready() const noexcept;
    void Close() noexcept;

private:
    friend class WaylandEglSurface;
    bool RegisterSurface(WaylandEglSurface &);
    void DetachSurface(WaylandEglSurface &) noexcept;
    void DiscoverCapabilities();

    wl_display *native_display_{};
    EGLDisplay egl_display_{EGL_NO_DISPLAY};
    EGLConfig egl_config_{};
    EGLContext egl_context_{EGL_NO_CONTEXT};
    std::vector<WaylandEglSurface *> surfaces_;
    std::uint64_t lifetime_id_{};
    PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC swap_damage_{};
    PFNEGLSETDAMAGEREGIONKHRPROC set_damage_region_{};
    bool ext_buffer_age_{};
    bool partial_update_{};
    bool initialized_{};
    bool closing_{};
};
} // namespace prism::platform
