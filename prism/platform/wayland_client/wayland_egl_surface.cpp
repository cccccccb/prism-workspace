#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/platform/wayland_egl_context.hpp"
#include <GLES3/gl3.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>
#include <wayland-client-core.h>
#include <wayland-egl.h>

namespace prism::platform {
namespace {
bool ConvertDamage(const contracts::DamageRegion &damage, int width, int height,
                   std::vector<EGLint> &rectangles)
{
    rectangles.clear();
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (damage.full) {
        return true; // EGL n_rects == 0 denotes the full surface.
    }
    if (damage.rects.size() > static_cast<std::size_t>(std::numeric_limits<EGLint>::max())) {
        return false;
    }
    rectangles.reserve(damage.rects.size() * 4);
    for (const auto &rectangle : damage.rects) {
        if (rectangle.width < 0 || rectangle.height < 0) {
            return false;
        }
        const auto left = std::clamp<std::int64_t>(rectangle.x, 0, width);
        const auto top = std::clamp<std::int64_t>(rectangle.y, 0, height);
        const auto right = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(rectangle.x) + rectangle.width, 0, width);
        const auto bottom = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(rectangle.y) + rectangle.height, 0, height);
        if (right <= left || bottom <= top) {
            continue;
        }
        rectangles.insert(rectangles.end(),
                          {static_cast<EGLint>(left), static_cast<EGLint>(height - bottom),
                           static_cast<EGLint>(right - left), static_cast<EGLint>(bottom - top)});
    }
    // EGL treats n_rects == 0 as full, so an empty non-full region must have a
    // rectangle whose clamped area is zero.
    if (rectangles.empty()) {
        rectangles = {0, 0, 0, 0};
    }
    return true;
}
} // namespace

WaylandEglSurface::~WaylandEglSurface()
{
    Close();
}

bool WaylandEglSurface::Open(WaylandEglContext &owner, wl_surface *surface, int width, int height)
{
    if (!owner.Ready() || owner.closing_ || owner_ || !surface || width <= 0 || height <= 0 ||
        width > 4096 || height > 4096 ||
        wl_proxy_get_display(reinterpret_cast<wl_proxy *>(surface)) != owner.native_display_) {
        return false;
    }

    owner_ = &owner;
    egl_window_ = wl_egl_window_create(surface, width, height);
    if (!egl_window_) {
        Close();
        return false;
    }
    egl_surface_ =
        eglCreateWindowSurface(owner.egl_display_, owner.egl_config_,
                               reinterpret_cast<EGLNativeWindowType>(egl_window_), nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
        Close();
        return false;
    }
    width_ = width;
    height_ = height;
    if (!owner.RegisterSurface(*this) || !MakeCurrent()) {
        Close();
        return false;
    }
    DiscoverDamageCapabilities();
    return true;
}

bool WaylandEglSurface::Ready() const noexcept
{
    return owner_ && owner_->Ready() && egl_surface_ != EGL_NO_SURFACE &&
           static_cast<bool>(target_identity_);
}

bool WaylandEglSurface::Resize(int width, int height)
{
    if (!Ready() || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return false;
    }
    if (width != width_ || height != height_) {
        // Changing the native size after declaring a partial region makes its
        // contents undefined. Keep the old size and let the caller close it.
        if (damage_region_set_ || frame_failed_ ||
            target_identity_.resize_generation == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        wl_egl_window_resize(egl_window_, width, height, 0, 0);
        width_ = width;
        height_ = height;
        ++target_identity_.resize_generation;
        frame_buffer_age_.reset();
        frame_dimensions_match_ = false;
        force_full_buffer_age_ = true;
    }
    return true;
}

bool WaylandEglSurface::MakeCurrent()
{
    if (!Ready() || eglMakeCurrent(owner_->egl_display_, egl_surface_, egl_surface_,
                                   owner_->egl_context_) != EGL_TRUE) {
        return false;
    }
    // Shared context state survives a surface switch. WSI draws into this
    // surface's default framebuffer, independently of any prior scratch FBO.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool WaylandEglSurface::QueryDimensions(EGLint &width, EGLint &height) const
{
    return Ready() &&
           eglQuerySurface(owner_->egl_display_, egl_surface_, EGL_WIDTH, &width) == EGL_TRUE &&
           eglQuerySurface(owner_->egl_display_, egl_surface_, EGL_HEIGHT, &height) == EGL_TRUE &&
           width > 0 && height > 0;
}

void WaylandEglSurface::DiscoverDamageCapabilities()
{
    capabilities_.swap_damage = owner_->swap_damage_ != nullptr;
    EGLint swap_behavior = EGL_NONE;
    capabilities_.partial_update = owner_->partial_update_ && owner_->set_damage_region_ &&
                                   eglQuerySurface(owner_->egl_display_, egl_surface_,
                                                   EGL_SWAP_BEHAVIOR, &swap_behavior) == EGL_TRUE &&
                                   swap_behavior == EGL_BUFFER_DESTROYED;
    // Without EXT, KHR only preserves pixels outside the declared repair. If
    // SetDamage cannot be used, its default full region leaves no preserved
    // pixels; returning a positive age would incorrectly permit partial draw.
    capabilities_.buffer_age = owner_->ext_buffer_age_ || capabilities_.partial_update;
}

std::optional<int> WaylandEglSurface::QueryBufferAge()
{
    if (!capabilities_.buffer_age || frame_failed_ || !MakeCurrent()) {
        return std::nullopt;
    }
    if (frame_buffer_age_) {
        return frame_buffer_age_;
    }
    EGLint width = 0, height = 0, age = 0;
    if (eglQuerySurface(owner_->egl_display_, egl_surface_, EGL_BUFFER_AGE_EXT, &age) != EGL_TRUE ||
        age < 0 || !QueryDimensions(width, height)) {
        return std::nullopt;
    }
    frame_dimensions_match_ = width == width_ && height == height_;
    // Native resize may not be reflected by EGL until the posting operation.
    // Both that case and an explicit resize require a full repair/first post.
    frame_buffer_age_ = force_full_buffer_age_ || !frame_dimensions_match_ ? 0 : age;
    return frame_buffer_age_;
}

DamageRegionResult WaylandEglSurface::FailFrame() noexcept
{
    frame_failed_ = true;
    return DamageRegionResult::Failed;
}

DamageRegionResult WaylandEglSurface::SetDamage(const contracts::DamageRegion &repair)
{
    if (!Ready() || frame_failed_) {
        return DamageRegionResult::Failed;
    }
    if (!capabilities_.partial_update) {
        return DamageRegionResult::Unsupported;
    }
    if (!MakeCurrent() || damage_region_set_) {
        return FailFrame();
    }
    const auto age = QueryBufferAge();
    if (!age || (!repair.full && (*age == 0 || !frame_dimensions_match_))) {
        return FailFrame();
    }
    std::vector<EGLint> rectangles;
    if (!ConvertDamage(repair, width_, height_, rectangles)) {
        return FailFrame();
    }
    // No GL draw/clear may precede this call. The renderer owns that ordering
    // and must clear/replay the entire repair region, even on implementations
    // which only expose the KHR age semantics (repair contents are undefined).
    damage_region_set_ = true;
    if (owner_->set_damage_region_(owner_->egl_display_, egl_surface_,
                                   rectangles.empty() ? nullptr : rectangles.data(),
                                   static_cast<EGLint>(rectangles.size() / 4)) != EGL_TRUE) {
        return FailFrame();
    }
    return DamageRegionResult::Applied;
}

bool WaylandEglSurface::Swap(const contracts::DamageRegion &content_damage)
{
    if (frame_failed_ || !MakeCurrent()) {
        return false;
    }
    std::vector<EGLint> rectangles;
    if (!ConvertDamage(content_damage, width_, height_, rectangles)) {
        return false;
    }
    bool full = content_damage.full || force_full_buffer_age_;
    if (!full && owner_->swap_damage_) {
        EGLint width = 0, height = 0;
        if (!QueryDimensions(width, height)) {
            return false;
        }
        full = width != width_ || height != height_;
    }
    const EGLBoolean result =
        owner_->swap_damage_ && !full
            ? owner_->swap_damage_(owner_->egl_display_, egl_surface_, rectangles.data(),
                                   static_cast<EGLint>(rectangles.size() / 4))
            : eglSwapBuffers(owner_->egl_display_, egl_surface_);
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

bool WaylandEglSurface::Swap()
{
    return Swap(contracts::DamageRegion::Full());
}

void WaylandEglSurface::Close() noexcept
{
    if (owner_) {
        owner_->DetachSurface(*this);
        if (egl_surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(owner_->egl_display_, egl_surface_);
        }
    }
    if (egl_window_) {
        wl_egl_window_destroy(egl_window_);
    }
    egl_surface_ = EGL_NO_SURFACE;
    owner_ = nullptr;
    target_identity_ = {};
    egl_window_ = nullptr;
    width_ = height_ = 0;
    capabilities_ = {};
    frame_buffer_age_.reset();
    frame_dimensions_match_ = false;
    force_full_buffer_age_ = true;
    damage_region_set_ = false;
    frame_failed_ = false;
}

std::string WaylandEglSurface::GlVendor() const
{
    auto *value = Ready() && eglGetCurrentDisplay() == owner_->egl_display_ &&
                          eglGetCurrentContext() == owner_->egl_context_
                      ? glGetString(GL_VENDOR)
                      : nullptr;
    return value ? reinterpret_cast<const char *>(value) : "";
}

std::string WaylandEglSurface::GlRenderer() const
{
    auto *value = Ready() && eglGetCurrentDisplay() == owner_->egl_display_ &&
                          eglGetCurrentContext() == owner_->egl_context_
                      ? glGetString(GL_RENDERER)
                      : nullptr;
    return value ? reinterpret_cast<const char *>(value) : "";
}

std::string WaylandEglSurface::GlVersion() const
{
    auto *value = Ready() && eglGetCurrentDisplay() == owner_->egl_display_ &&
                          eglGetCurrentContext() == owner_->egl_context_
                      ? glGetString(GL_VERSION)
                      : nullptr;
    return value ? reinterpret_cast<const char *>(value) : "";
}
} // namespace prism::platform
