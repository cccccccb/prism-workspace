#include "prism/platform/wayland_egl_surface.hpp"
#include <GLES3/gl3.h>
#include <wayland-egl.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace prism::platform {
namespace {
bool HasExtension(std::string_view extensions, std::string_view extension) {
    std::size_t offset = 0;
    while (offset < extensions.size()) {
        while (offset < extensions.size() &&
               std::isspace(static_cast<unsigned char>(extensions[offset]))) ++offset;
        const auto begin = offset;
        while (offset < extensions.size() &&
               !std::isspace(static_cast<unsigned char>(extensions[offset]))) ++offset;
        if (extensions.substr(begin, offset - begin) == extension) return true;
    }
    return false;
}

bool ConvertDamage(const contracts::DamageRegion& damage, int width, int height,
                   std::vector<EGLint>& rectangles) {
    rectangles.clear();
    if (width <= 0 || height <= 0) return false;
    if (damage.full) return true; // EGL n_rects == 0 denotes the full surface.
    if (damage.rects.size() > static_cast<std::size_t>(std::numeric_limits<EGLint>::max()))
        return false;
    rectangles.reserve(damage.rects.size() * 4);
    for (const auto& rectangle : damage.rects) {
        if (rectangle.width < 0 || rectangle.height < 0) return false;
        const auto left = std::clamp<std::int64_t>(rectangle.x, 0, width);
        const auto top = std::clamp<std::int64_t>(rectangle.y, 0, height);
        const auto right = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(rectangle.x) + rectangle.width, 0, width);
        const auto bottom = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(rectangle.y) + rectangle.height, 0, height);
        if (right <= left || bottom <= top) continue;
        rectangles.insert(rectangles.end(), {
            static_cast<EGLint>(left), static_cast<EGLint>(height - bottom),
            static_cast<EGLint>(right - left), static_cast<EGLint>(bottom - top)});
    }
    // EGL treats n_rects == 0 as full, so an empty non-full region must have a
    // rectangle whose clamped area is zero.
    if (rectangles.empty()) rectangles = {0, 0, 0, 0};
    return true;
}
} // namespace

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
    if (!MakeCurrent()) { Close(); return false; }
    DiscoverDamageCapabilities();
    return true;
}
bool WaylandEglSurface::Resize(int width, int height) {
    if (!Ready() || width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    if (width != width_ || height != height_) {
        // Changing the native size after declaring a partial region makes its
        // contents undefined. Keep the old size and let the caller close it.
        if (damage_region_set_ || frame_failed_) return false;
        wl_egl_window_resize(egl_window_, width, height, 0, 0);
        width_ = width;
        height_ = height;
        frame_buffer_age_.reset();
        frame_dimensions_match_ = false;
        force_full_buffer_age_ = true;
    }
    return true;
}
bool WaylandEglSurface::MakeCurrent() {
    return Ready() && eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_) == EGL_TRUE;
}

bool WaylandEglSurface::QueryDimensions(EGLint& width, EGLint& height) const {
    return Ready() && eglQuerySurface(egl_display_, egl_surface_, EGL_WIDTH, &width) == EGL_TRUE &&
        eglQuerySurface(egl_display_, egl_surface_, EGL_HEIGHT, &height) == EGL_TRUE &&
        width > 0 && height > 0;
}

void WaylandEglSurface::DiscoverDamageCapabilities() {
    const auto* extension_string = eglQueryString(egl_display_, EGL_EXTENSIONS);
    const std::string_view extensions = extension_string ? extension_string : "";
    const bool partial_update = HasExtension(extensions, "EGL_KHR_partial_update");
    if (HasExtension(extensions, "EGL_KHR_swap_buffers_with_damage")) {
        swap_damage_ = reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(
            eglGetProcAddress("eglSwapBuffersWithDamageKHR"));
    }
    if (!swap_damage_ && HasExtension(extensions, "EGL_EXT_swap_buffers_with_damage")) {
        swap_damage_ = reinterpret_cast<PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC>(
            eglGetProcAddress("eglSwapBuffersWithDamageEXT"));
    }
    capabilities_.swap_damage = swap_damage_ != nullptr;
    if (partial_update) {
        set_damage_region_ = reinterpret_cast<PFNEGLSETDAMAGEREGIONKHRPROC>(
            eglGetProcAddress("eglSetDamageRegionKHR"));
    }
    EGLint swap_behavior = EGL_NONE;
    capabilities_.partial_update = set_damage_region_ &&
        eglQuerySurface(egl_display_, egl_surface_, EGL_SWAP_BEHAVIOR, &swap_behavior) == EGL_TRUE &&
        swap_behavior == EGL_BUFFER_DESTROYED;
    // Without EXT, KHR only preserves pixels outside the declared repair. If
    // SetDamage cannot be used, its default full region leaves no preserved
    // pixels; returning a positive age would incorrectly permit partial draw.
    capabilities_.buffer_age = HasExtension(extensions, "EGL_EXT_buffer_age") ||
        capabilities_.partial_update;
}

std::optional<int> WaylandEglSurface::QueryBufferAge() {
    if (!capabilities_.buffer_age || frame_failed_ || !MakeCurrent()) return std::nullopt;
    if (frame_buffer_age_) return frame_buffer_age_;
    EGLint width = 0, height = 0, age = 0;
    if (eglQuerySurface(egl_display_, egl_surface_, EGL_BUFFER_AGE_EXT, &age) != EGL_TRUE || age < 0 ||
        !QueryDimensions(width, height))
        return std::nullopt;
    frame_dimensions_match_ = width == width_ && height == height_;
    // Native resize may not be reflected by EGL until the posting operation.
    // Both that case and an explicit resize require a full repair/first post.
    frame_buffer_age_ = force_full_buffer_age_ || !frame_dimensions_match_ ? 0 : age;
    return frame_buffer_age_;
}

DamageRegionResult WaylandEglSurface::SetDamage(const contracts::DamageRegion& repair) {
    const auto failed = [this] {
        frame_failed_ = true;
        return DamageRegionResult::Failed;
    };
    if (!Ready() || frame_failed_) return DamageRegionResult::Failed;
    if (!capabilities_.partial_update) return DamageRegionResult::Unsupported;
    if (!MakeCurrent() || damage_region_set_) return failed();
    const auto age = QueryBufferAge();
    if (!age || (!repair.full && (*age == 0 || !frame_dimensions_match_)))
        return failed();
    std::vector<EGLint> rectangles;
    if (!ConvertDamage(repair, width_, height_, rectangles)) return failed();
    // No GL draw/clear may precede this call. The renderer owns that ordering
    // and must clear/replay the entire repair region, even on implementations
    // which only expose the KHR age semantics (repair contents are undefined).
    damage_region_set_ = true;
    if (set_damage_region_(egl_display_, egl_surface_,
            rectangles.empty() ? nullptr : rectangles.data(),
            static_cast<EGLint>(rectangles.size() / 4)) != EGL_TRUE) {
        return failed();
    }
    return DamageRegionResult::Applied;
}

bool WaylandEglSurface::Swap(const contracts::DamageRegion& content_damage) {
    if (frame_failed_ || !MakeCurrent()) return false;
    std::vector<EGLint> rectangles;
    if (!ConvertDamage(content_damage, width_, height_, rectangles)) return false;
    bool full = content_damage.full || force_full_buffer_age_;
    if (!full && swap_damage_) {
        EGLint width = 0, height = 0;
        if (!QueryDimensions(width, height)) return false;
        full = width != width_ || height != height_;
    }
    const EGLBoolean result = swap_damage_ && !full
        ? swap_damage_(egl_display_, egl_surface_, rectangles.data(),
                       static_cast<EGLint>(rectangles.size() / 4))
        : eglSwapBuffers(egl_display_, egl_surface_);
    if (result != EGL_TRUE) {
        // Never retry a failed posting operation: the buffer/frame boundary is
        // unknown, and a second swap could submit another frame.
        frame_failed_ = true;
        return false;
    }
    frame_buffer_age_.reset();
    frame_dimensions_match_ = false;
    force_full_buffer_age_ = false;
    damage_region_set_ = false;
    return true;
}

bool WaylandEglSurface::Swap() { return Swap(contracts::DamageRegion::Full()); }

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
    capabilities_ = {};
    swap_damage_ = nullptr;
    set_damage_region_ = nullptr;
    frame_buffer_age_.reset();
    frame_dimensions_match_ = false;
    force_full_buffer_age_ = true;
    damage_region_set_ = false;
    frame_failed_ = false;
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
