#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace prism::render {

class FrameBuffer {
public:
    FrameBuffer(int width, int height);

    void Clear(uint32_t color);
    void DrawDesktopGradient();

    void DrawRect(int x, int y, int w, int h, uint32_t color);
    void DrawRoundedRect(int x, int y, int w, int h, float radius, uint32_t color, float opacity = 1.0f);
    void DrawBorder(int x, int y, int w, int h, float radius, float stroke_width, uint32_t color);
    void DrawCircle(int cx, int cy, int radius, uint32_t color);
    void DrawGlow(int x, int y, int w, int h, float radius, uint32_t color);
    void DrawShadow(int x, int y, int w, int h, float radius, float shadow_radius, uint32_t color);
    void ApplyKawaseBlur(int x, int y, int w, int h, float blur_radius, int passes);
    void Blit(const FrameBuffer& src, int dst_x, int dst_y, int dst_w, int dst_h, float opacity = 1.0f);
    void CopyRegion(const FrameBuffer& src, int src_x, int src_y, int dst_x, int dst_y, int w, int h);

    // Simple bitmap font / icon stamping for visual confirmation
    void DrawTextSimple(int x, int y, const std::string& text, uint32_t color, int scale = 1);

    // macOS Desktop Chrome & Shell components
    void DrawTopMenuBar(const std::string& active_app, const std::string& time_str);
    void DrawMacDock(const std::vector<std::string>& app_names, int active_index);
    void DrawSplitDivider(int x, int y, int h, bool is_dragged);
    void DrawCursor(int x, int y);
    void DrawMissionControlSpaces(float progress);
    void DrawDebugHud(float fps, float frame_time_ms, int frame_count, const std::string& mode_str, int cursor_x, int cursor_y);

    bool SavePPM(const std::string& filepath) const;
    bool LoadPPM(const std::string& filepath);
    bool LoadImage(const std::string& filepath);

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    const uint32_t* GetPixels() const { return pixels_.data(); }
    uint32_t* GetPixelsMutable() { return pixels_.data(); }

private:
    int width_;
    int height_;
    std::vector<uint32_t> pixels_;
};

} // namespace prism::render
