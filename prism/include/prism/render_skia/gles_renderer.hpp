#pragma once
#include "prism/contracts/damage.hpp"
#include "prism/contracts/display_list.hpp"
#include <cstddef>
#include <memory>

namespace prism::render_skia {
class RasterRenderer;

struct GlesRendererOptions {
    // Soft budget for Ganesh resources; EGL/driver and live allocations may exceed it.
    std::size_t resource_cache_bytes{32u * 1024u * 1024u};
};

struct GlesRenderStats {
    std::uint64_t full_renders{}, partial_renders{}, empty_renders{}, repair_pixels{};
    std::uint64_t image_upload_attempts{}, image_upload_successes{}, image_uploaded_bytes{};
};

// Draws a validated DisplayList into the current GLES framebuffer.
// EGL context, surface and buffer presentation belong to the platform layer.
class GlesRenderer {
public:
    explicit GlesRenderer(const RasterRenderer &commands, GlesRendererOptions options = {});
    ~GlesRenderer();
    GlesRenderer(const GlesRenderer &) = delete;
    GlesRenderer &operator=(const GlesRenderer &) = delete;
    bool Ready() const;
    // Requires the creation EGL context current. Success means a real backend
    // texture exists and its upload was submitted, not that the GPU is finished.
    bool UploadImage(contracts::ResourceId id);
    bool ImageUploaded(contracts::ResourceId id) const;
    void ReleaseImage(contracts::ResourceId id);
    // Raw backend callers may synchronously upload missing images here. SDK
    // applications preupload under their owner-turn count/byte/time budgets.
    bool Render(const contracts::DisplayList &list, int width, int height);
    bool Render(const contracts::DisplayList &list, int width, int height,
                const contracts::DamageRegion &repair);
    GlesRenderStats GetRenderStats() const;
    // Release GPU objects with their creation context current. If that context
    // is unavailable, Abandon forgets the objects without deleting another
    // context's identically numbered GL resources.
    void Close();
    void Abandon();

private:
    bool EnsureImages(const contracts::DisplayList &list);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::render_skia
