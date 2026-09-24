#pragma once

#include "prism/render/framebuffer.hpp"
#include <string>
#include <vector>
#include <memory>

namespace prism::desktop {

struct WallpaperItem {
    std::string id;
    std::string name;
    std::string theme;
    std::string filepath;
};

class WallpaperManager {
public:
    WallpaperManager(const std::string& wallpaper_dir = "");

    void AddWallpaper(const std::string& id, const std::string& name, const std::string& theme, const std::string& filepath);
    bool NextWallpaper();
    bool PrevWallpaper();
    bool SetWallpaperById(const std::string& id);
    bool SetWallpaperByIndex(size_t index);

    const WallpaperItem* GetCurrentWallpaper() const;
    size_t GetCurrentIndex() const { return current_index_; }
    size_t GetTotalCount() const { return wallpapers_.size(); }

    // Renders the current wallpaper into destination framebuffer
    void RenderTo(render::FrameBuffer& dst);

private:
    void RenderProcedural(render::FrameBuffer& dst, const std::string& theme);

    std::string wallpaper_dir_;
    std::vector<WallpaperItem> wallpapers_;
    size_t current_index_{0};
    std::unique_ptr<render::FrameBuffer> cached_surface_;
    size_t cached_index_{static_cast<size_t>(-1)};
};

} // namespace prism::desktop
