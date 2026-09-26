#include "prism/platform/wayland_egl_surface.hpp"
#include <GLES3/gl3.h>
#include <wayland-egl.h>
#include <algorithm>

namespace prism::platform {
WaylandEglSurface::~WaylandEglSurface() { Close(); }

bool WaylandEglSurface::Open(wl_display* display, wl_surface* surface, int width, int height) {
    if (!display || !surface || width <= 0 || height <= 0 || width > 4096 || height > 4096 || Ready())
        return false;
    egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display));
    if (egl_display_ == EGL_NO_DISPLAY || !eglInitialize(egl_display_, nullptr, nullptr)) {
        Close();
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) { Close(); return false; }
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(egl_display_, attributes, &config, 1, &count) || count != 1) {
        Close(); return false;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, config, EGL_NO_CONTEXT, context_attributes);
    if (egl_context_ == EGL_NO_CONTEXT) { Close(); return false; }
    egl_window_ = wl_egl_window_create(surface, width, height);
    if (!egl_window_) { Close(); return false; }
    egl_surface_ = eglCreateWindowSurface(egl_display_, config,
        reinterpret_cast<EGLNativeWindowType>(egl_window_), nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) { Close(); return false; }
    width_ = width;
    height_ = height;
    return MakeCurrent();
}
bool WaylandEglSurface::Resize(int width, int height) {
    if (!Ready() || width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    if (width != width_ || height != height_) {
        wl_egl_window_resize(egl_window_, width, height, 0, 0);
        width_ = width;
        height_ = height;
    }
    return true;
}
bool WaylandEglSurface::MakeCurrent() {
    return Ready() && eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_) == EGL_TRUE;
}
bool WaylandEglSurface::Swap() {
    return MakeCurrent() && eglSwapBuffers(egl_display_, egl_surface_) == EGL_TRUE;
}
void WaylandEglSurface::Close() {
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface_ != EGL_NO_SURFACE) eglDestroySurface(egl_display_, egl_surface_);
        if (egl_context_ != EGL_NO_CONTEXT) eglDestroyContext(egl_display_, egl_context_);
    }
    if (egl_window_) wl_egl_window_destroy(egl_window_);
    if (egl_display_ != EGL_NO_DISPLAY) eglTerminate(egl_display_);
    egl_surface_ = EGL_NO_SURFACE;
    egl_context_ = EGL_NO_CONTEXT;
    egl_display_ = EGL_NO_DISPLAY;
    egl_window_ = nullptr;
    width_ = height_ = 0;
}
std::string WaylandEglSurface::GlVendor() const {
    auto* value = Ready() && eglGetCurrentContext() == egl_context_ ? glGetString(GL_VENDOR) : nullptr;
    return value ? reinterpret_cast<const char*>(value) : "";
}
std::string WaylandEglSurface::GlRenderer() const {
    auto* value = Ready() && eglGetCurrentContext() == egl_context_ ? glGetString(GL_RENDERER) : nullptr;
    return value ? reinterpret_cast<const char*>(value) : "";
}
std::string WaylandEglSurface::GlVersion() const {
    auto* value = Ready() && eglGetCurrentContext() == egl_context_ ? glGetString(GL_VERSION) : nullptr;
    return value ? reinterpret_cast<const char*>(value) : "";
}
} // namespace prism::platform
