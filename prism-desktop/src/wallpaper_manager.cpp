#include "wallpaper_manager.hpp"
#include "prism/core/logging.hpp"
#include <filesystem>
#include <cmath>

namespace prism::desktop {

WallpaperManager::WallpaperManager(const std::string& wallpaper_dir)
    : wallpaper_dir_(wallpaper_dir) {
    if (wallpaper_dir_.empty()) {
        if (std::filesystem::exists("resources/wallpapers")) {
            wallpaper_dir_ = "resources/wallpapers";
        } else if (std::filesystem::exists("../resources/wallpapers")) {
            wallpaper_dir_ = "../resources/wallpapers";
        }
    }

    // Register presets
    AddWallpaper("sunset_anime", "Sunset Anime Coastline", "Anime", wallpaper_dir_ + "/sunset_anime.png");
    AddWallpaper("ocean_sunset", "Tropical Sunset Beach", "Sunset", wallpaper_dir_ + "/ocean_sunset.jpg");
    AddWallpaper("deep_space", "Cosmic Purple Nebula", "DeepSpace", wallpaper_dir_ + "/deep_space.jpg");
    AddWallpaper("cyber_night", "Cyberpunk Neon Tokyo", "Cyber", wallpaper_dir_ + "/cyber_night.jpg");

    PRISM_LOG_INFO("DESKTOP", "WallpaperManager initialized with %zu wallpapers from '%s'",
                   wallpapers_.size(), wallpaper_dir_.c_str());
}

void WallpaperManager::AddWallpaper(const std::string& id, const std::string& name, const std::string& theme, const std::string& filepath) {
    wallpapers_.push_back({id, name, theme, filepath});
}

bool WallpaperManager::NextWallpaper() {
    if (wallpapers_.empty()) return false;
    current_index_ = (current_index_ + 1) % wallpapers_.size();
    PRISM_LOG_INFO("DESKTOP", "Switched to wallpaper [%zu/%zu]: '%s' (%s)",
                   current_index_ + 1, wallpapers_.size(), wallpapers_[current_index_].name.c_str(), wallpapers_[current_index_].theme.c_str());
    return true;
}

bool WallpaperManager::PrevWallpaper() {
    if (wallpapers_.empty()) return false;
    current_index_ = (current_index_ + wallpapers_.size() - 1) % wallpapers_.size();
    PRISM_LOG_INFO("DESKTOP", "Switched to wallpaper [%zu/%zu]: '%s' (%s)",
                   current_index_ + 1, wallpapers_.size(), wallpapers_[current_index_].name.c_str(), wallpapers_[current_index_].theme.c_str());
    return true;
}

bool WallpaperManager::SetWallpaperById(const std::string& id) {
    for (size_t i = 0; i < wallpapers_.size(); ++i) {
        if (wallpapers_[i].id == id) {
            current_index_ = i;
            return true;
        }
    }
    return false;
}

bool WallpaperManager::SetWallpaperByIndex(size_t index) {
    if (index < wallpapers_.size()) {
        current_index_ = index;
        return true;
    }
    return false;
}

const WallpaperItem* WallpaperManager::GetCurrentWallpaper() const {
    if (current_index_ < wallpapers_.size()) {
        return &wallpapers_[current_index_];
    }
    return nullptr;
}

void WallpaperManager::RenderTo(render::FrameBuffer& dst) {
    const auto* cur = GetCurrentWallpaper();
    if (!cur) {
        dst.DrawDesktopGradient();
        return;
    }

    if (cached_index_ == current_index_ && cached_surface_ &&
        cached_surface_->GetWidth() == dst.GetWidth() && cached_surface_->GetHeight() == dst.GetHeight()) {
        dst.Blit(*cached_surface_, 0, 0, dst.GetWidth(), dst.GetHeight());
        return;
    }

    // Try loading image from disk
    bool loaded = false;
    if (!cur->filepath.empty() && std::filesystem::exists(cur->filepath)) {
        if (!cached_surface_ || cached_surface_->GetWidth() != dst.GetWidth() || cached_surface_->GetHeight() != dst.GetHeight()) {
            cached_surface_ = std::make_unique<render::FrameBuffer>(dst.GetWidth(), dst.GetHeight());
        }
        loaded = cached_surface_->LoadImage(cur->filepath);
    }

    if (loaded && cached_surface_) {
        cached_index_ = current_index_;
        dst.Blit(*cached_surface_, 0, 0, dst.GetWidth(), dst.GetHeight());
    } else {
        // Fallback procedural rendering
        RenderProcedural(dst, cur->theme);
    }
}

void WallpaperManager::RenderProcedural(render::FrameBuffer& dst, const std::string& theme) {
    int w = dst.GetWidth();
    int h = dst.GetHeight();

    if (theme == "Sunset" || theme == "Anime") {
        for (int y = 0; y < h; ++y) {
            float t = static_cast<float>(y) / static_cast<float>(h);
            uint8_t r = static_cast<uint8_t>(255 * (1.0f - t * 0.5f));
            uint8_t g = static_cast<uint8_t>(120 * (1.0f - t * 0.7f) + 40 * t);
            uint8_t b = static_cast<uint8_t>(180 * (1.0f - t) + 80 * t);
            uint32_t c = (0xFF << 24) | (r << 16) | (g << 8) | b;
            for (int x = 0; x < w; ++x) dst.GetPixelsMutable()[y * w + x] = c;
        }
    } else if (theme == "Cyber") {
        for (int y = 0; y < h; ++y) {
            float t = static_cast<float>(y) / static_cast<float>(h);
            uint8_t r = static_cast<uint8_t>(20 + 60 * t);
            uint8_t g = static_cast<uint8_t>(10 + 20 * t);
            uint8_t b = static_cast<uint8_t>(40 + 80 * (1.0f - t));
            uint32_t c = (0xFF << 24) | (r << 16) | (g << 8) | b;
            for (int x = 0; x < w; ++x) dst.GetPixelsMutable()[y * w + x] = c;
        }
    } else {
        dst.DrawDesktopGradient();
    }
}

} // namespace prism::desktop
