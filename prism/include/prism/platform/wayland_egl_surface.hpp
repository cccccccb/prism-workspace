#pragma once
#include "prism/contracts/damage.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <optional>
#include <string>

struct wl_display;
struct wl_surface;
struct wl_egl_window;

namespace prism::platform {

struct EglDamageCapabilities {
    // Usable preservation, not merely an advertised query attribute. KHR-only
    // age requires an Applied SetDamage before drawing; EXT does not.
    bool buffer_age{false};
    bool swap_damage{false};
    bool partial_update{false};
};

enum class DamageRegionResult { Applied, Unsupported, Failed };

// WSI owns GPU buffers and wl_buffer handoff. Use only on the Wayland/UI thread.
class WaylandEglSurface {
public:
    WaylandEglSurface() = default;
    ~WaylandEglSurface();
    WaylandEglSurface(const WaylandEglSurface &) = delete;
    WaylandEglSurface &operator=(const WaylandEglSurface &) = delete;

    bool Open(wl_display *display, wl_surface *surface, int width, int height);
    bool Resize(int width, int height);
    bool MakeCurrent();

    const EglDamageCapabilities &Capabilities() const
    {
        return capabilities_;
    }

    // Query before GPU drawing. Unsupported/query failure is nullopt; zero
    // requires full repair. Creation/resize stays zero until a successful swap.
    // With KHR-only support, SetDamage must succeed before any partial draw.
    std::optional<int> QueryBufferAge();
    // Buffer repair is relative to this back buffer's last use, not the last
    // submitted frame. Call at most once, before any GPU draw in this frame.
    // Applied requires every draw/clear to remain within repair; Failed is
    // terminal for this frame. Unsupported leaves the default full region.
    DamageRegionResult SetDamage(const contracts::DamageRegion &repair);
    // Surface content damage is relative to the last submitted frame. Empty
    // non-full means no changed pixels; full is authoritative. Swap failure
    // must not be retried with a second posting operation.
    bool Swap(const contracts::DamageRegion &content_damage);
    bool Swap();
    void Close();
    std::string GlVendor() const;
    std::string GlRenderer() const;
    std::string GlVersion() const;

    bool Ready() const
    {
        return egl_surface_ != EGL_NO_SURFACE;
    }

private:
    bool QueryDimensions(EGLint &width, EGLint &height) const;
    void DiscoverDamageCapabilities();

    EGLDisplay egl_display_{EGL_NO_DISPLAY};
    EGLContext egl_context_{EGL_NO_CONTEXT};
    EGLSurface egl_surface_{EGL_NO_SURFACE};
    wl_egl_window *egl_window_{nullptr};
    int width_{0};
    int height_{0};
    EglDamageCapabilities capabilities_{};
    PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC swap_damage_{nullptr};
    PFNEGLSETDAMAGEREGIONKHRPROC set_damage_region_{nullptr};
    std::optional<int> frame_buffer_age_;
    bool frame_dimensions_match_{false};
    bool force_full_buffer_age_{true};
    bool damage_region_set_{false};
    bool frame_failed_{false};
};

} // namespace prism::platform
