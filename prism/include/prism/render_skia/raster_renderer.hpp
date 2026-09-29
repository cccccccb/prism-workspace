#pragma once
#include "prism/contracts/damage.hpp"
#include "prism/contracts/display_list.hpp"
#include "prism/runtime/image_resources.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
class SkCanvas;
class SkImage;

namespace prism::render_skia {
namespace detail {
class ImageProvider;
class ResourceTable;
} // namespace detail
class GlesRenderer;

// Diagnostic CPU backend and display-list damage analyzer.
// No Skia types escape its public API.
class RasterRenderer {
public:
    explicit RasterRenderer(std::string font_path);
    ~RasterRenderer();
    RasterRenderer(const RasterRenderer &) = delete;
    RasterRenderer &operator=(const RasterRenderer &) = delete;

    bool Ready() const;
    bool RegisterFont(contracts::ResourceId id, const std::string &path);
    bool RegisterImage(contracts::ResourceId id, const runtime::DecodedImage &image);
    // The lease binds immutable RGBA bytes to the Skia raster data lifetime.
    bool RegisterImage(contracts::ResourceId id, runtime::ImageLease image);
    void UnregisterImage(contracts::ResourceId id);
    bool Render(const contracts::DisplayList &list, void *pixels, int width, int height,
                int stride) const;
    // Compare against the last successfully submitted list/resource epoch.
    // Unknown structure or paint bounds conservatively require full damage.
    contracts::DamageRegion CompareDamage(const contracts::DisplayList *previous,
                                          const contracts::DisplayList &next, int width, int height,
                                          std::uint64_t previous_resource_epoch) const;
    std::uint64_t ResourceEpoch() const;
    bool Render(const contracts::DisplayList &list, void *pixels, int width, int height, int stride,
                const contracts::DamageRegion &repair) const;

    contracts::ResourceId FontId() const
    {
        return default_font_;
    }

private:
    friend class GlesRenderer;
    // Preserve the exact declared union; repair consumers may never widen it
    // via producer fragmentation/area policies after WSI SetDamage.
    static std::optional<contracts::DamageRegion> ClipRepair(const contracts::DamageRegion &,
                                                             int width, int height);
    static bool Replay(const contracts::DisplayList &list, SkCanvas *canvas, int width, int height,
                       const contracts::DamageRegion &repair,
                       const detail::ResourceTable &resources,
                       const detail::ImageProvider *images = nullptr);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    contracts::ResourceId default_font_{1};
};

} // namespace prism::render_skia
