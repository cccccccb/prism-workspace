#pragma once
#include "prism/contracts/damage.hpp"
#include "prism/contracts/display_list.hpp"
#include "prism/contracts/gpu_target.hpp"
#include "prism/runtime/image_resources.hpp"
#include <cstddef>
#include <memory>
#include <string>

namespace prism::render_skia {
struct GlesRendererOptions {
    // Soft budget for Ganesh resources; EGL/driver and live allocations may exceed it.
    std::size_t resource_cache_bytes{32u * 1024u * 1024u};
};

struct GlesRenderStats {
    std::uint64_t full_renders{}, partial_renders{}, empty_renders{}, repair_pixels{};
    std::uint64_t image_upload_attempts{}, image_upload_successes{}, image_uploaded_bytes{};
    std::uint64_t target_wraps{}, target_cache_hits{}, target_switches{}, target_releases{};
};

// Draws a validated DisplayList into the current GLES framebuffer.
// EGL context, surface and buffer presentation belong to the platform layer.
class GlesRenderer {
public:
    explicit GlesRenderer(std::string font_path, GlesRendererOptions options = {});
    GlesRenderer(std::string font_path, contracts::GpuTargetIdentity target,
                 GlesRendererOptions options = {});
    ~GlesRenderer();
    GlesRenderer(const GlesRenderer &) = delete;
    GlesRenderer &operator=(const GlesRenderer &) = delete;
    bool Ready() const;
    bool RegisterFont(contracts::ResourceId id, const std::string &path);
    bool RegisterImage(contracts::ResourceId id, const runtime::DecodedImage &image);
    bool RegisterImage(contracts::ResourceId id, runtime::ImageLease image);
    void UnregisterImage(contracts::ResourceId id);
    // Requires the creation EGL context current. Success means a real backend
    // texture exists and its upload was submitted, not that the GPU is finished.
    bool UploadImage(contracts::ResourceId id);
    bool ImageUploaded(contracts::ResourceId id) const;
    void ReleaseImage(contracts::ResourceId id);
    // Compatibility calls are restricted to the creation draw/read EGL surface.
    // Raw callers may synchronously upload images; SDK owners preupload them.
    bool Render(const contracts::DisplayList &list, int width, int height);
    bool Render(const contracts::DisplayList &list, int width, int height,
                const contracts::DamageRegion &repair);
    // One context shares resources across up to 64 live targets. A target owns
    // its wrapper and damage baseline; the caller declares the actual repair.
    // The same surface lifetime must retain its native draw/read surfaces and
    // advance its generation before changing dimensions or framebuffer storage.
    bool Render(const contracts::DisplayList &list, contracts::GpuTargetIdentity target, int width,
                int height);
    bool Render(const contracts::DisplayList &list, contracts::GpuTargetIdentity target, int width,
                int height, const contracts::DamageRegion &repair);
    // Call with the creation context current before destroying the target WSI.
    // Released identities must not be registered again; a new lifetime gets a
    // new ID. A stale generation cannot release its live replacement.
    bool ReleaseTarget(contracts::GpuTargetIdentity target);
    GlesRenderStats GetRenderStats() const;
    // Release GPU objects with their creation context current. If that context
    // is unavailable, Abandon forgets the objects without deleting another
    // context's identically numbered GL resources.
    void Close();
    void Abandon();

private:
    bool RenderInternal(const contracts::DisplayList &list,
                        const contracts::GpuTargetIdentity *target, int width, int height,
                        const contracts::DamageRegion &repair);
    bool EnsureImages(const contracts::DisplayList &list);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::render_skia
