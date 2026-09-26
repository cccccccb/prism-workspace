#pragma once
#include <EGL/egl.h>
#include <string>

struct wl_display;
struct wl_surface;
struct wl_egl_window;

namespace prism::platform {

// WSI owns GPU buffers and wl_buffer handoff. Use only on the Wayland/UI thread.
class WaylandEglSurface {
public:
    WaylandEglSurface() = default;
    ~WaylandEglSurface();
    WaylandEglSurface(const WaylandEglSurface&) = delete;
    WaylandEglSurface& operator=(const WaylandEglSurface&) = delete;

    bool Open(wl_display* display, wl_surface* surface, int width, int height);
    bool Resize(int width, int height);
    bool MakeCurrent();
    bool Swap();
    void Close();
    std::string GlVendor() const;
    std::string GlRenderer() const;
    std::string GlVersion() const;
    bool Ready() const { return egl_surface_ != EGL_NO_SURFACE; }

private:
    EGLDisplay egl_display_{EGL_NO_DISPLAY};
    EGLContext egl_context_{EGL_NO_CONTEXT};
    EGLSurface egl_surface_{EGL_NO_SURFACE};
    wl_egl_window* egl_window_{nullptr};
    int width_{0};
    int height_{0};
};

} // namespace prism::platform
