#pragma once
#include "prism/contracts/display_list.hpp"
#include <memory>

namespace prism::render_skia {
class RasterRenderer;

// Draws a validated DisplayList into the current GLES framebuffer.
// EGL context, surface and buffer presentation belong to the platform layer.
class GlesRenderer {
public:
    explicit GlesRenderer(const RasterRenderer& commands);
    ~GlesRenderer();
    GlesRenderer(const GlesRenderer&) = delete;
    GlesRenderer& operator=(const GlesRenderer&) = delete;
    bool Ready() const;
    bool Render(const contracts::DisplayList& list, int width, int height);
    void Close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::render_skia
