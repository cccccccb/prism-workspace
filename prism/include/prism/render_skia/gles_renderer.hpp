#pragma once
#include "prism/contracts/display_list.hpp"
#include <memory>
#include <cstddef>

namespace prism::render_skia {
class RasterRenderer;

struct GlesRendererOptions {
    // Soft budget for Ganesh resources; EGL/driver and live allocations may exceed it.
    std::size_t resource_cache_bytes{32u * 1024u * 1024u};
};

// Draws a validated DisplayList into the current GLES framebuffer.
// EGL context, surface and buffer presentation belong to the platform layer.
class GlesRenderer {
public:
    explicit GlesRenderer(const RasterRenderer& commands, GlesRendererOptions options = {});
    ~GlesRenderer();
    GlesRenderer(const GlesRenderer&) = delete;
    GlesRenderer& operator=(const GlesRenderer&) = delete;
    bool Ready() const;
    bool Render(const contracts::DisplayList& list, int width, int height);
    // Release GPU objects with their creation context current. If that context
    // is unavailable, Abandon forgets the objects without deleting another
    // context's identically numbered GL resources.
    void Close();
    void Abandon();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::render_skia
