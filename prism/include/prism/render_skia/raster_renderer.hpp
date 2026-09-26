#pragma once
#include "prism/contracts/display_list.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/image_resources.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace prism::render_skia {

// Diagnostic CPU backend. No Skia types escape this API; GPU backends will
// consume the same DisplayList after a target-device comparison.
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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::render_skia
