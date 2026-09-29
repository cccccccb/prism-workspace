#include "prism/runtime/text_shaper.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <hb-ft.h>
#include <unordered_map>

namespace prism::runtime {

struct TextShaper::Impl {
    FT_Library freetype{nullptr};
    std::unordered_map<std::uint64_t, FT_Face> fonts;

    ~Impl()
    {
        for (auto &[id, face] : fonts) {
            FT_Done_Face(face);
        }
        if (freetype) {
            FT_Done_FreeType(freetype);
        }
    }
};

TextShaper::TextShaper(std::string font_path) : impl_(std::make_unique<Impl>())
{
    if (FT_Init_FreeType(&impl_->freetype) != 0) {
        return;
    }
    RegisterFont(default_font_, font_path);
}

TextShaper::~TextShaper() = default;

bool TextShaper::Ready() const
{
    return impl_->fonts.contains(default_font_.value);
}

bool TextShaper::RegisterFont(contracts::ResourceId id, const std::string &path)
{
    if (!id || !impl_->freetype || impl_->fonts.contains(id.value)) {
        return false;
    }

    FT_Face face = nullptr;
    if (FT_New_Face(impl_->freetype, path.c_str(), 0, &face) != 0) {
        return false;
    }

    impl_->fonts.emplace(id.value, face);
    return true;
}

ShapedText TextShaper::Shape(std::string_view text, double size)
{
    return Shape(default_font_, text, size);
}

ShapedText TextShaper::Shape(contracts::ResourceId id, std::string_view text, double size)
{
    ShapedText result;
    auto entry = impl_->fonts.find(id.value);
    if (entry == impl_->fonts.end() || !std::isfinite(size) || size <= 0 || size > 512) {
        return result;
    }
    if (FT_Set_Char_Size(entry->second, 0, static_cast<FT_F26Dot6>(size * 64), 0, 0) != 0) {
        return result;
    }
    hb_font_t *font = hb_ft_font_create_referenced(entry->second);
    if (!font) {
        return result;
    }

    hb_buffer_t *buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0,
                       static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, nullptr, 0);
    unsigned count = 0;
    auto *infos = hb_buffer_get_glyph_infos(buffer, &count);
    auto *positions = hb_buffer_get_glyph_positions(buffer, &count);
    const double ascent = entry->second->size->metrics.ascender / 64.0;
    const double descent = -entry->second->size->metrics.descender / 64.0;
    double cursor_x = 0, cursor_y = ascent;
    result.glyphs.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        result.glyphs.push_back(
            {infos[i].codepoint,
             {cursor_x + positions[i].x_offset / 64.0, cursor_y - positions[i].y_offset / 64.0}});
        cursor_x += positions[i].x_advance / 64.0;
        cursor_y -= positions[i].y_advance / 64.0;
    }
    result.width = std::max(0.0, cursor_x);
    result.height = ascent + descent;
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    return result;
}

} // namespace prism::runtime
