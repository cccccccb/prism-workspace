#pragma once
#include "prism/contracts/display_list.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/image_resources.hpp"
#include <memory>
#include <string>
#include <string_view>
class SkCanvas;

namespace prism::render_skia {
class GlesRenderer;

// Diagnostic CPU backend and shared command/font/image owner for GLES.
// No Skia types escape its public API.
class RasterRenderer {
public:
    explicit RasterRenderer(std::string font_path);
    ~RasterRenderer();
    RasterRenderer(const RasterRenderer&) = delete;
    RasterRenderer& operator=(const RasterRenderer&) = delete;

    bool Ready() const;
    runtime::ShapedText Shape(std::string_view text, double size) const;
    static std::optional<runtime::DecodedImage> DecodePng(const std::string& path);
    bool RegisterImage(contracts::ResourceId id, const runtime::DecodedImage& image);
    bool Render(const contracts::DisplayList& list, void* pixels,
                int width, int height, int stride) const;
    contracts::ResourceId FontId() const { return contracts::ResourceId{1}; }

private:
    friend class GlesRenderer;
    bool Replay(const contracts::DisplayList& list, SkCanvas* canvas) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::render_skia
