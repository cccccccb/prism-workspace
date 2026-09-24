#include "prism/render/framebuffer.hpp"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace prism::render {

static inline uint8_t GetA(uint32_t c) { return (c >> 24) & 0xFF; }
static inline uint8_t GetR(uint32_t c) { return (c >> 16) & 0xFF; }
static inline uint8_t GetG(uint32_t c) { return (c >> 8) & 0xFF; }
static inline uint8_t GetB(uint32_t c) { return c & 0xFF; }

static inline uint32_t MakeRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8)  |
           static_cast<uint32_t>(b);
}

static inline uint32_t AlphaBlend(uint32_t dst, uint32_t src, float factor = 1.0f) {
    uint32_t sa = (src >> 24) & 0xFF;
    if (factor < 1.0f) {
        if (factor <= 0.0f) return dst;
        sa = static_cast<uint32_t>(sa * factor);
    }
    if (sa == 0) return dst;
    if (sa == 255) return src;

    uint32_t inv_a = 255 - sa;

    // Fast integer SIMD-in-a-register blending for 0xAARRGGBB
    // Blend Red (bits 16-23) and Blue (bits 0-7) simultaneously
    uint32_t src_rb = src & 0x00FF00FF;
    uint32_t dst_rb = dst & 0x00FF00FF;
    uint32_t out_rb = (((src_rb * sa + dst_rb * inv_a + 0x00800080) >> 8) & 0x00FF00FF);

    // Blend Green (bits 8-15)
    uint32_t src_g = src & 0x0000FF00;
    uint32_t dst_g = dst & 0x0000FF00;
    uint32_t out_g = (((src_g * sa + dst_g * inv_a + 0x00008000) >> 8) & 0x0000FF00);

    // Composite alpha channel
    uint32_t dst_a = (dst >> 24) & 0xFF;
    uint32_t out_a = sa + ((dst_a * inv_a + 128) >> 8);
    if (out_a > 255) out_a = 255;

    return (out_a << 24) | out_rb | out_g;
}

FrameBuffer::FrameBuffer(int width, int height)
    : width_(width), height_(height), pixels_(width * height, 0xFF000000) {}

void FrameBuffer::Clear(uint32_t color) {
    std::fill(pixels_.begin(), pixels_.end(), color);
}

void FrameBuffer::DrawDesktopGradient() {
    // macOS Dark Nebula style subtle diagonal gradient
    for (int y = 0; y < height_; ++y) {
        float ny = static_cast<float>(y) / height_;
        for (int x = 0; x < width_; ++x) {
            float nx = static_cast<float>(x) / width_;
            float t = (nx + ny) * 0.5f;

            // Gradient: Deep midnight blue (#0b0c10) to dark purple/slate (#1f2833)
            uint8_t r = static_cast<uint8_t>(11 + (31 - 11) * t);
            uint8_t g = static_cast<uint8_t>(12 + (40 - 12) * t);
            uint8_t b = static_cast<uint8_t>(20 + (55 - 20) * t);

            pixels_[y * width_ + x] = MakeRgba(r, g, b, 255);
        }
    }
}

void FrameBuffer::DrawRect(int x, int y, int w, int h, uint32_t color) {
    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(width_, x + w);
    int y1 = std::min(height_, y + h);
    if (x0 >= x1 || y0 >= y1) return;

    uint8_t a = (color >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 255) {
        for (int py = y0; py < y1; ++py) {
            uint32_t* row = &pixels_[py * width_ + x0];
            std::fill_n(row, x1 - x0, color);
        }
        return;
    }

    for (int py = y0; py < y1; ++py) {
        uint32_t* row = &pixels_[py * width_];
        for (int px = x0; px < x1; ++px) {
            row[px] = AlphaBlend(row[px], color);
        }
    }
}

void FrameBuffer::DrawRoundedRect(int x, int y, int w, int h, float radius, uint32_t color, float opacity) {
    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(width_, x + w);
    int y1 = std::min(height_, y + h);
    if (x0 >= x1 || y0 >= y1 || opacity <= 0.0f) return;

    int r = static_cast<int>(radius);
    if (r <= 0) {
        DrawRect(x, y, w, h, color);
        return;
    }

    float r2 = radius * radius;
    float r_plus_1_sq = (radius + 1.0f) * (radius + 1.0f);

    int left_corner = x + r;
    int right_corner = x + w - r;
    int top_corner = y + r;
    int bottom_corner = y + h - r;

    for (int py = y0; py < y1; ++py) {
        bool is_top = (py < top_corner);
        bool is_bottom = (py >= bottom_corner);
        bool is_vert_corner = is_top || is_bottom;

        float dy = 0.0f;
        if (is_top) {
            dy = (top_corner - 0.5f) - py;
        } else if (is_bottom) {
            dy = py - (bottom_corner - 0.5f);
        }

        uint32_t* row = &pixels_[py * width_];

        for (int px = x0; px < x1; ++px) {
            bool is_left = (px < left_corner);
            bool is_right = (px >= right_corner);

            if (is_vert_corner && (is_left || is_right)) {
                float dx = is_left ? ((left_corner - 0.5f) - px) : (px - (right_corner - 0.5f));
                float dsq = dx * dx + dy * dy;
                if (dsq <= r2) {
                    row[px] = AlphaBlend(row[px], color, opacity);
                } else if (dsq < r_plus_1_sq) {
                    float dist = std::sqrt(dsq);
                    float edge = (radius + 1.0f - dist) * opacity;
                    if (edge > 0.0f) {
                        row[px] = AlphaBlend(row[px], color, edge);
                    }
                }
            } else {
                // Interior rectangle (95%+ of pixels) - fast direct blend
                row[px] = AlphaBlend(row[px], color, opacity);
            }
        }
    }
}

void FrameBuffer::DrawCircle(int cx, int cy, int radius, uint32_t color) {
    if (radius <= 0) return;
    int x0 = std::max(0, cx - radius);
    int y0 = std::max(0, cy - radius);
    int x1 = std::min(width_, cx + radius + 1);
    int y1 = std::min(height_, cy + radius + 1);
    if (x0 >= x1 || y0 >= y1) return;

    float r2 = static_cast<float>(radius * radius);
    float r_edge = static_cast<float>((radius + 1) * (radius + 1));

    for (int py = y0; py < y1; ++py) {
        float dy = static_cast<float>(py - cy);
        float dy2 = dy * dy;
        uint32_t* row = &pixels_[py * width_];
        for (int px = x0; px < x1; ++px) {
            float dx = static_cast<float>(px - cx);
            float d2 = dx * dx + dy2;
            if (d2 <= r2) {
                row[px] = AlphaBlend(row[px], color);
            } else if (d2 < r_edge) {
                float dist = std::sqrt(d2);
                float alpha_t = (radius + 1.0f - dist);
                row[px] = AlphaBlend(row[px], color, alpha_t);
            }
        }
    }
}

void FrameBuffer::DrawBorder(int x, int y, int w, int h, float radius, float stroke_width, uint32_t color) {
    if (w <= 0 || h <= 0 || stroke_width <= 0.0f) return;
    int sw = std::max(1, static_cast<int>(std::ceil(stroke_width)));
    DrawRoundedRect(x, y, w, sw, radius, color);
    DrawRoundedRect(x, y + h - sw, w, sw, radius, color);
    DrawRoundedRect(x, y, sw, h, radius, color);
    DrawRoundedRect(x + w - sw, y, sw, h, radius, color);
}

void FrameBuffer::DrawGlow(int x, int y, int w, int h, float radius, uint32_t color) {
    int g = static_cast<int>(radius);
    if (g <= 0) return;
    int x0 = std::max(0, x - g);
    int y0 = std::max(0, y - g);
    int x1 = std::min(width_, x + w + g);
    int y1 = std::min(height_, y + h + g);
    if (x0 >= x1 || y0 >= y1) return;

    float inv_g = 1.0f / radius;
    for (int py = y0; py < y1; ++py) {
        float dy = 0.0f;
        if (py < y) dy = static_cast<float>(y - py);
        else if (py >= y + h) dy = static_cast<float>(py - (y + h));

        uint32_t* row = &pixels_[py * width_];
        for (int px = x0; px < x1; ++px) {
            float dx = 0.0f;
            if (px < x) dx = static_cast<float>(x - px);
            else if (px >= x + w) dx = static_cast<float>(px - (x + w));

            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist <= radius) {
                float t = 1.0f - dist * inv_g;
                float falloff = t * t * 0.45f;
                row[px] = AlphaBlend(row[px], color, falloff);
            }
        }
    }
}

void FrameBuffer::DrawShadow(int x, int y, int w, int h, float radius, float shadow_radius, uint32_t color) {
    int s = static_cast<int>(shadow_radius);
    if (s <= 0) return;

    int x0 = std::max(0, x - s);
    int y0 = std::max(0, y - s);
    int x1 = std::min(width_, x + w + s);
    int y1 = std::min(height_, y + h + s);
    if (x0 >= x1 || y0 >= y1) return;

    int r = static_cast<int>(radius);
    int left_bound = x + r;
    int right_bound = x + w - r;
    int top_bound = y + r;
    int bottom_bound = y + h - r;

    float inv_s = 1.0f / shadow_radius;

    for (int py = y0; py < y1; ++py) {
        bool inside_y = (py >= y && py < y + h);

        bool is_top = (py < top_bound);
        bool is_bottom = (py >= bottom_bound);
        float dy = 0.0f;
        if (is_top) dy = static_cast<float>(top_bound - py);
        else if (is_bottom) dy = static_cast<float>(py - bottom_bound);

        uint32_t* row = &pixels_[py * width_];

        for (int px = x0; px < x1; ++px) {
            if (inside_y && px >= x && px < x + w) continue;

            bool is_left = (px < left_bound);
            bool is_right = (px >= right_bound);
            float dist = 0.0f;

            if ((is_top || is_bottom) && (is_left || is_right)) {
                // Radial 4 outer corners
                float dx = is_left ? static_cast<float>(left_bound - px) : static_cast<float>(px - right_bound);
                dist = std::sqrt(dx * dx + dy * dy) - radius;
            } else if (is_top) {
                dist = static_cast<float>(y - py);
            } else if (is_bottom) {
                dist = static_cast<float>(py - (y + h));
            } else if (is_left) {
                dist = static_cast<float>(x - px);
            } else if (is_right) {
                dist = static_cast<float>(px - (x + w));
            }

            if (dist > 0.0f && dist <= shadow_radius) {
                float t = 1.0f - dist * inv_s;
                float falloff = t * t * 0.35f;
                row[px] = AlphaBlend(row[px], color, falloff);
            }
        }
    }
}

void FrameBuffer::Blit(const FrameBuffer& src, int dst_x, int dst_y, int dst_w, int dst_h, float opacity) {
    if (dst_w <= 0 || dst_h <= 0 || opacity <= 0.0f) return;
    int src_w = src.GetWidth();
    int src_h = src.GetHeight();
    if (src_w <= 0 || src_h <= 0) return;

    for (int y = 0; y < dst_h; ++y) {
        int target_y = dst_y + y;
        if (target_y < 0 || target_y >= height_) continue;
        int src_y = (y * src_h) / dst_h;
        if (src_y >= src_h) src_y = src_h - 1;

        for (int x = 0; x < dst_w; ++x) {
            int target_x = dst_x + x;
            if (target_x < 0 || target_x >= width_) continue;
            int src_x = (x * src_w) / dst_w;
            if (src_x >= src_w) src_x = src_w - 1;

            uint32_t src_color = src.GetPixels()[src_y * src_w + src_x];
            int dst_idx = target_y * width_ + target_x;
            pixels_[dst_idx] = AlphaBlend(pixels_[dst_idx], src_color, opacity);
        }
    }
}

void FrameBuffer::CopyRegion(const FrameBuffer& src, int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int sw = src.GetWidth();
    int sh = src.GetHeight();
    const uint32_t* src_px = src.GetPixels();
    if (!src_px || pixels_.empty()) return;

    for (int y = 0; y < h; ++y) {
        int sy = src_y + y;
        int dy = dst_y + y;
        if (sy < 0 || sy >= sh || dy < 0 || dy >= height_) continue;
        for (int x = 0; x < w; ++x) {
            int sx = src_x + x;
            int dx = dst_x + x;
            if (sx < 0 || sx >= sw || dx < 0 || dx >= width_) continue;
            pixels_[dy * width_ + dx] = src_px[sy * sw + sx];
        }
    }
}

void FrameBuffer::ApplyKawaseBlur(int x, int y, int w, int h, float blur_radius, int passes) {
    (void)blur_radius;
    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(width_, x + w);
    int y1 = std::min(height_, y + h);
    int bw = x1 - x0;
    int bh = y1 - y0;
    if (bw <= 8 || bh <= 8) return;

    int ds_scale = (bw >= 256 && bh >= 256) ? 8 : 4;
    int ds_w = bw / ds_scale;
    int ds_h = bh / ds_scale;
    if (ds_w <= 0 || ds_h <= 0) return;

    static thread_local std::vector<uint32_t> ds_buf;
    static thread_local std::vector<uint32_t> ds_temp;
    if (static_cast<int>(ds_buf.size()) < ds_w * ds_h) {
        ds_buf.resize(ds_w * ds_h);
        ds_temp.resize(ds_w * ds_h);
    }

    // Downsample ds_scale x: fast row-strided copy
    for (int dy = 0; dy < ds_h; ++dy) {
        const uint32_t* src_row = &pixels_[(y0 + dy * ds_scale) * width_ + x0];
        uint32_t* dst_row = &ds_buf[dy * ds_w];
        for (int dx = 0; dx < ds_w; ++dx) {
            dst_row[dx] = src_row[dx * ds_scale];
        }
    }

    // 2-pass blur in low-resolution space with SIMD-in-a-register 32-bit math
    int actual_passes = std::min(passes, 2);
    for (int pass = 1; pass <= actual_passes; ++pass) {
        int off = pass;
        for (int dy = 0; dy < ds_h; ++dy) {
            int ym = std::max(0, dy - off);
            int yp = std::min(ds_h - 1, dy + off);
            const uint32_t* row_ym = &ds_buf[ym * ds_w];
            const uint32_t* row_yp = &ds_buf[yp * ds_w];
            uint32_t* dst_row = &ds_temp[dy * ds_w];

            for (int dx = 0; dx < ds_w; ++dx) {
                int xm = std::max(0, dx - off);
                int xp = std::min(ds_w - 1, dx + off);

                uint32_t c1 = row_ym[xm];
                uint32_t c2 = row_ym[xp];
                uint32_t c3 = row_yp[xm];
                uint32_t c4 = row_yp[xp];

                // Fast parallel average of 4 ARGB pixels
                uint32_t rb = (((c1 & 0x00FF00FF) + (c2 & 0x00FF00FF) + (c3 & 0x00FF00FF) + (c4 & 0x00FF00FF)) >> 2) & 0x00FF00FF;
                uint32_t ga = ((((c1 >> 8) & 0x00FF00FF) + ((c2 >> 8) & 0x00FF00FF) + ((c3 >> 8) & 0x00FF00FF) + ((c4 >> 8) & 0x00FF00FF)) >> 2) & 0x00FF00FF;
                dst_row[dx] = rb | (ga << 8);
            }
        }
        std::memcpy(ds_buf.data(), ds_temp.data(), ds_w * ds_h * sizeof(uint32_t));
    }

    // Upsample back to pixels_
    for (int py = y0; py < y1; ++py) {
        int dy = std::min(ds_h - 1, (py - y0) / ds_scale);
        const uint32_t* src_row = &ds_buf[dy * ds_w];
        uint32_t* dst_row = &pixels_[py * width_ + x0];
        for (int px = 0; px < bw; ++px) {
            dst_row[px] = src_row[px / ds_scale];
        }
    }
}

#include "prism/render/font5x7.inl"

void FrameBuffer::DrawTextSimple(int x, int y, const std::string& text, uint32_t color, int scale) {
    if (scale <= 1) {
        for (size_t i = 0; i < text.size(); ++i) {
            char ch = text[i];
            if (ch < 32 || ch > 126) ch = ' ';
            int idx = ch - 32;
            int cx = x + static_cast<int>(i) * 7;
            if (cx + 6 >= width_ || y + 8 >= height_) break;

            const uint8_t* cols = FONT_5X7[idx];
            for (int c = 0; c < 5; ++c) {
                uint8_t col_bits = cols[c];
                for (int r = 0; r < 7; ++r) {
                    if ((col_bits >> r) & 1) {
                        int px = cx + c;
                        int py = y + r;
                        if (px >= 0 && px < width_ && py >= 0 && py < height_) {
                            pixels_[py * width_ + px] = AlphaBlend(pixels_[py * width_ + px], color);
                        }
                    }
                }
            }
        }
    } else {
        int pitch = 7 * scale;
        for (size_t i = 0; i < text.size(); ++i) {
            char ch = text[i];
            if (ch < 32 || ch > 126) ch = ' ';
            int idx = ch - 32;
            int cx = x + static_cast<int>(i) * pitch;
            if (cx + 5 * scale >= width_ || y + 7 * scale >= height_) break;

            const uint8_t* cols = FONT_5X7[idx];
            for (int c = 0; c < 5; ++c) {
                uint8_t col_bits = cols[c];
                for (int r = 0; r < 7; ++r) {
                    if ((col_bits >> r) & 1) {
                        int px0 = cx + c * scale;
                        int py0 = y + r * scale;
                        for (int sy = 0; sy < scale; ++sy) {
                            int py = py0 + sy;
                            if (py < 0 || py >= height_) continue;
                            uint32_t* row = &pixels_[py * width_];
                            for (int sx = 0; sx < scale; ++sx) {
                                int px = px0 + sx;
                                if (px >= 0 && px < width_) {
                                    row[px] = AlphaBlend(row[px], color);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void FrameBuffer::DrawTopMenuBar(const std::string& active_app, const std::string& time_str) {
    int bar_h = (height_ >= 38) ? 34 : height_;
    ApplyKawaseBlur(0, 0, width_, bar_h, 20.0f, 3);
    DrawRect(0, 0, width_, bar_h, 0xB0121622); // Luxury dark frosted glass tint
    DrawRect(0, 0, width_, 1, 0x38FFFFFF);      // Specular rim at top
    DrawRect(0, bar_h - 1, width_, 1, 0x25FFFFFF); // Inner separator line

    // Outer soft drop shadow below the TopBar onto windows/desktop
    if (height_ >= bar_h + 4) {
        DrawRect(0, bar_h, width_, 1, 0x35000000);
        DrawRect(0, bar_h + 1, width_, 1, 0x20000000);
        DrawRect(0, bar_h + 2, width_, 1, 0x10000000);
        DrawRect(0, bar_h + 3, width_, 1, 0x05000000);
    }

    auto DrawShadowedText = [this](int tx, int ty, const std::string& str, uint32_t col, int sc = 1) {
        DrawTextSimple(tx, ty + 1, str, 0x88000000, sc);
        DrawTextSimple(tx, ty, str, col, sc);
    };

    // 1. Left: Prism circular emblem [●] & Brand text
    int cy = bar_h / 2;
    DrawCircle(20, cy, 10, 0x30E04040);
    DrawCircle(20, cy, 7, 0xFFE04040);
    DrawCircle(20, cy, 3, 0xFFFFFFFF);
    DrawShadowedText(34, cy - 4, active_app.empty() ? "PRISM" : active_app, 0xFFFFFFFF, 1);

    // 2. Center: Mac Dynamic Island Clock Pill with top handle
    std::string display_time = time_str.empty() ? "Oct-11 15:13:24" : time_str;
    int pill_w = 184;
    int pill_h = 24;
    int pill_x = (width_ - pill_w) / 2;
    int pill_y = (bar_h - pill_h) / 2;

    DrawShadow(pill_x, pill_y, pill_w, pill_h, 12.0f, 6.0f, 0x77000000);
    DrawRoundedRect(pill_x, pill_y, pill_w, pill_h, 12.0f, 0xF20B0E14);
    DrawBorder(pill_x, pill_y, pill_w, pill_h, 12.0f, 1.0f, 0x33FFFFFF);
    DrawRoundedRect((width_ - 40) / 2, pill_y + 2, 40, 2, 1.0f, 0xCCFFFFFF);

    int text_x = pill_x + (pill_w - static_cast<int>(display_time.size()) * 7) / 2;
    DrawShadowedText(text_x, pill_y + 9, display_time, 0xFFFFFFFF, 1);

    // 3. Right: System control center pills (Wi-Fi, Battery, Bell)
    if (width_ > 500) {
        DrawRoundedRect(width_ - 216, cy - 9, 74, 18, 5.0f, 0x22FFFFFF);
        DrawBorder(width_ - 216, cy - 9, 74, 18, 5.0f, 1.0f, 0x20FFFFFF);
        DrawShadowedText(width_ - 208, cy - 4, "Wi-Fi 5G", 0xEEFFFFFF, 1);

        DrawRoundedRect(width_ - 132, cy - 9, 78, 18, 5.0f, 0x22FFFFFF);
        DrawBorder(width_ - 132, cy - 9, 78, 18, 5.0f, 1.0f, 0x20FFFFFF);
        DrawRoundedRect(width_ - 126, cy - 6, 24, 12, 3.0f, 0x33FFFFFF);
        DrawRoundedRect(width_ - 124, cy - 4, 20, 8, 2.0f, 0xFF30D158);
        DrawShadowedText(width_ - 96, cy - 4, "100%", 0xFFFFFFFF, 1);

        DrawCircle(width_ - 34, cy, 10, 0x22FFFFFF);
        DrawBorder(width_ - 44, cy - 10, 20, 20, 10.0f, 1.0f, 0x20FFFFFF);
        DrawShadowedText(width_ - 38, cy - 4, "[*]", 0xFFE0E6ED, 1);
    }
}

void FrameBuffer::DrawMacDock(const std::vector<std::string>& app_names, int active_index) {
    int count = static_cast<int>(app_names.size());
    if (count == 0) return;

    int pad = (width_ >= 500 && height_ >= 90) ? 16 : 0;
    int dock_x = pad;
    int dock_y = pad;
    int dock_w = width_ - 2 * pad;
    int dock_h = height_ - 2 * pad;

    int icon_size = 46;
    int gap = 12;
    int pad_h = 16;

    // 1. Frosted Glass Backdrop Blur (blurs the underlying wallpaper inside dock body)
    ApplyKawaseBlur(dock_x, dock_y, dock_w, dock_h, 24.0f, 4);

    // 2. Multi-layer Photorealistic Outer Drop Shadow
    if (pad > 0) {
        DrawShadow(dock_x, dock_y + 4, dock_w, dock_h, 22.0f, static_cast<float>(pad), 0x77000000);
        DrawShadow(dock_x, dock_y + 1, dock_w, dock_h, 22.0f, 8.0f, 0x44000000);
    }

    // 3. Frosted Acrylic Glass Body
    DrawRoundedRect(dock_x, dock_y, dock_w, dock_h, 22.0f, 0xB8161A26, 0.95f);

    // 4. Inner Specular Rims & Shadows
    DrawBorder(dock_x, dock_y, dock_w, dock_h, 22.0f, 1.0f, 0x38FFFFFF);
    DrawRoundedRect(dock_x + 22, dock_y, dock_w - 44, 1, 1.0f, 0x48FFFFFF);
    DrawRoundedRect(dock_x + 22, dock_y + dock_h - 2, dock_w - 44, 1, 1.0f, 0x22000000);

    int cur_x = dock_x + pad_h;
    int card_y = dock_y + (dock_h - icon_size) / 2;

    // 5. [田] App Launcher Card
    DrawShadow(cur_x, card_y + 1, icon_size, icon_size, 11.0f, 5.0f, 0x55000000);
    DrawRoundedRect(cur_x, card_y, icon_size, icon_size, 11.0f, 0x30FFFFFF);
    DrawBorder(cur_x, card_y, icon_size, icon_size, 11.0f, 1.0f, 0x30FFFFFF);
    DrawRoundedRect(cur_x + 9, card_y + 9, 10, 10, 2.0f, 0xFFFFFFFF);
    DrawRoundedRect(cur_x + 27, card_y + 9, 10, 10, 2.0f, 0xFFFFFFFF);
    DrawRoundedRect(cur_x + 9, card_y + 27, 10, 10, 2.0f, 0xFFFFFFFF);
    DrawRoundedRect(cur_x + 27, card_y + 27, 10, 10, 2.0f, 0xFFFFFFFF);
    cur_x += icon_size + 12;

    // 6. Vertical Glass Separator
    DrawRect(cur_x, dock_y + 16, 2, dock_h - 32, 0x30FFFFFF);
    cur_x += 2 + 12;

    // 7. Squircle App Icons & Running Dots
    uint32_t colors[6] = {0xFF007AFF, 0xFF2C3240, 0xFF34C759, 0xFFFF9500, 0xFFAF52DE, 0xFFFA2D48};

    auto DrawShadowedText = [this](int tx, int ty, const std::string& str, uint32_t col, int sc = 1) {
        DrawTextSimple(tx, ty + 1, str, 0x88000000, sc);
        DrawTextSimple(tx, ty, str, col, sc);
    };

    for (int i = 0; i < count; ++i) {
        uint32_t c = colors[i % 6];
        // 3D icon drop shadow
        DrawShadow(cur_x, card_y + 2, icon_size, icon_size, 11.0f, 6.0f, 0x66000000);
        // Squircle body
        DrawRoundedRect(cur_x, card_y, icon_size, icon_size, 11.0f, c);
        // Top inner specular sheen
        DrawRoundedRect(cur_x + 6, card_y + 1, icon_size - 12, 1, 1.0f, 0x40FFFFFF);
        DrawBorder(cur_x, card_y, icon_size, icon_size, 11.0f, 1.0f, 0x20FFFFFF);

        std::string label = (app_names[i].size() > 5) ? app_names[i].substr(0, 4) : app_names[i];
        int lbl_x = cur_x + (icon_size - static_cast<int>(label.size()) * 7) / 2;
        DrawShadowedText(lbl_x, card_y + 19, label, 0xFFFFFFFF, 1);

        // Running indicator dot / pill
        if (i == active_index) {
            DrawShadow(cur_x + icon_size / 2 - 4, dock_y + dock_h - 6, 8, 4, 2.0f, 3.0f, 0x66007AFF);
            DrawRoundedRect(cur_x + icon_size / 2 - 3, dock_y + dock_h - 6, 6, 3, 1.5f, 0xFFFFFFFF);
        } else if (i < 3) {
            DrawRoundedRect(cur_x + icon_size / 2 - 3, dock_y + dock_h - 6, 6, 3, 1.5f, 0xAAFFFFFF);
        }
        cur_x += icon_size + gap;
    }
}

void FrameBuffer::DrawSplitDivider(int x, int y, int h, bool is_dragged) {
    uint32_t line_color = is_dragged ? 0x88007AFF : 0x22FFFFFF;
    DrawRect(x - 1, y, 2, h, line_color);

    int cy = y + h / 2;
    int pill_w = 6;
    int pill_h = 42;
    uint32_t pill_color = is_dragged ? 0xFF007AFF : 0xAAFFFFFF;
    DrawRoundedRect(x - pill_w / 2, cy - pill_h / 2, pill_w, pill_h, 3.0f, pill_color);
}

void FrameBuffer::DrawCursor(int x, int y) {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) return;
    static const uint16_t CURSOR_BITS[18] = {
        0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xFF80, 0xFFC0, 0xFCE0, 0xF0E0, 0xE070, 0xC070, 0x8038, 0x0038,
        0x001C, 0x001C
    };
    static const uint16_t OUTLINE_BITS[18] = {
        0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80,
        0xFFC0, 0xFFE0, 0xFFF0, 0xFFF0, 0xF0F8, 0xE0F8, 0xC07C, 0x807C,
        0x003E, 0x003E
    };

    for (int r = 0; r < 18; ++r) {
        for (int c = 0; c < 12; ++c) {
            int px = x + c;
            int py = y + r;
            if (px >= 0 && px < width_ && py >= 0 && py < height_) {
                if ((OUTLINE_BITS[r] >> (15 - c)) & 1) {
                    if ((CURSOR_BITS[r] >> (15 - c)) & 1) {
                        pixels_[py * width_ + px] = 0xFF141416;
                    } else {
                        pixels_[py * width_ + px] = 0xFFFFFFFF;
                    }
                }
            }
        }
    }
}

void FrameBuffer::DrawMissionControlSpaces(float progress) {
    if (progress <= 0.05f) return;
    float overlay_alpha = progress * 0.45f;
    DrawRect(0, 30, width_, height_ - 30, MakeRgba(0, 0, 0, static_cast<uint8_t>(overlay_alpha * 255)));

    int bar_y = 35;
    int bar_h = 65;
    int bar_w = 640;
    int bar_x = (width_ - bar_w) / 2;

    ApplyKawaseBlur(bar_x, bar_y, bar_w, bar_h, 16.0f, 3);
    DrawRoundedRect(bar_x, bar_y, bar_w, bar_h, 14.0f, 0x551a1e28, progress);
    DrawRoundedRect(bar_x, bar_y, bar_w, bar_h, 14.0f, 0x22ffffff, progress * 0.3f);

    DrawTextSimple(bar_x + 30, bar_y + 26, "Desktop 1 (Split)", 0xFFFFFFFF);
    DrawTextSimple(bar_x + 220, bar_y + 26, "Desktop 2 (Music)", 0xCCAAAAAA);
    DrawTextSimple(bar_x + 410, bar_y + 26, "Desktop 3 (+)", 0x88AAAAAA);
}

void FrameBuffer::DrawDebugHud(float fps, float frame_time_ms, int frame_count, const std::string& mode_str, int cursor_x, int cursor_y) {
    int scale = (width_ >= 1600) ? 2 : 1;
    int hud_w = (scale == 2) ? 460 : 330;
    int hud_h = (scale == 2) ? 172 : 110;
    int hud_x = 18;
    int hud_y = 44; // Placed just below top menu bar (height 30)

    // Semi-transparent dark slate backdrop with soft shadow and rounded corners
    DrawShadow(hud_x, hud_y, hud_w, hud_h, 12.0f, 20.0f, 0xAA000000);
    DrawRoundedRect(hud_x, hud_y, hud_w, hud_h, 12.0f, 0xF00D1117, 0.95f);
    DrawRoundedRect(hud_x, hud_y, hud_w, (scale == 2 ? 3 : 2), 2.0f, 0xFF58A6FF); // top accent highlight

    int pad_x = hud_x + (scale == 2 ? 18 : 12);
    int line_h = (scale == 2) ? 30 : 18;
    int cur_y = hud_y + (scale == 2 ? 16 : 12);

    // Header line
    DrawTextSimple(pad_x, cur_y, "[PRISM-WM DEBUG HUD]", 0xFF58A6FF, scale);
    cur_y += line_h;

    // FPS Display (Color-coded: Green >= 50, Yellow >= 30, Red < 30)
    uint32_t fps_color = (fps >= 50.0f) ? 0xFF3FB950 : ((fps >= 30.0f) ? 0xFFD29922 : 0xFFF85149);
    char fps_buf[64];
    snprintf(fps_buf, sizeof(fps_buf), "FPS: %.1f  (Frame: %.2f ms)  #%d", fps, frame_time_ms, frame_count);
    DrawTextSimple(pad_x, cur_y, fps_buf, fps_color, scale);
    cur_y += line_h;

    // Display mode
    std::string disp_line = "Display: " + (mode_str.empty() ? "Native Output" : mode_str);
    DrawTextSimple(pad_x, cur_y, disp_line, 0xFFE6EDF3, scale);
    cur_y += line_h;

    // Hardware cursor tracking
    char cur_buf[64];
    snprintf(cur_buf, sizeof(cur_buf), "Cursor: X=%d Y=%d [HW OK]", cursor_x, cursor_y);
    DrawTextSimple(pad_x, cur_y, cur_buf, 0xFF7EE787, scale);
    cur_y += line_h;

    // Engine architecture
    DrawTextSimple(pad_x, cur_y, "Engine: wlroots 0.17 | Native VSync", 0xFF8B949E, scale);
}

bool FrameBuffer::SavePPM(const std::string& filepath) const {
    std::ofstream out(filepath, std::ios::binary);
    if (!out) return false;

    out << "P6\n" << width_ << " " << height_ << "\n255\n";
    std::vector<uint8_t> rgb(width_ * height_ * 3);

    for (size_t i = 0; i < pixels_.size(); ++i) {
        rgb[i * 3 + 0] = GetR(pixels_[i]);
        rgb[i * 3 + 1] = GetG(pixels_[i]);
        rgb[i * 3 + 2] = GetB(pixels_[i]);
    }

    out.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    return true;
}

bool FrameBuffer::LoadPPM(const std::string& filepath) {
    std::ifstream in(filepath, std::ios::binary);
    if (!in) return false;

    std::string magic;
    in >> magic;
    if (magic != "P6") return false;

    auto skip_ws_and_comments = [&in]() {
        while (true) {
            int c = in.peek();
            if (std::isspace(c)) {
                in.get();
            } else if (c == '#') {
                std::string line;
                std::getline(in, line);
            } else {
                break;
            }
        }
    };

    skip_ws_and_comments();
    int w = 0, h = 0, maxval = 0;
    in >> w;
    skip_ws_and_comments();
    in >> h;
    skip_ws_and_comments();
    in >> maxval;
    in.get(); // Consume single newline or space separator

    if (w <= 0 || h <= 0 || maxval <= 0 || maxval > 255) return false;

    width_ = w;
    height_ = h;
    pixels_.resize(w * h);

    std::vector<uint8_t> rgb(w * h * 3);
    in.read(reinterpret_cast<char*>(rgb.data()), rgb.size());
    if (!in) return false;

    for (int i = 0; i < w * h; ++i) {
        uint8_t r = rgb[i * 3 + 0];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        pixels_[i] = (0xFF << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
    return true;
}

bool FrameBuffer::LoadImage(const std::string& filepath) {
    if (filepath.ends_with(".ppm")) {
        return LoadPPM(filepath);
    }
    // Check if a preconverted .ppm exists alongside it
    auto dot_pos = filepath.find_last_of('.');
    if (dot_pos != std::string::npos) {
        std::string ppm_path = filepath.substr(0, dot_pos) + ".ppm";
        if (std::filesystem::exists(ppm_path)) {
            return LoadPPM(ppm_path);
        }
    }
    // Convert on demand via Python PIL into a cached ppm file in /tmp
    std::string cached_ppm = "/tmp/prism_cache_" + std::to_string(std::hash<std::string>{}(filepath)) + ".ppm";
    if (!std::filesystem::exists(cached_ppm)) {
        std::string cmd = "python3 -c \"from PIL import Image; Image.open('" + filepath + "').convert('RGB').save('" + cached_ppm + "')\" 2>/dev/null";
        int ret = std::system(cmd.c_str());
        if (ret != 0) return false;
    }
    return LoadPPM(cached_ppm);
}

} // namespace prism::render
