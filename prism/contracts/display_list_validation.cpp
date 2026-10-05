#include "prism/contracts/display_list_validation.hpp"
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace prism::contracts {
namespace {
bool ValidRect(LogicalRect rect)
{
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
           std::isfinite(rect.height) && rect.width >= 0 && rect.height >= 0 &&
           std::abs(rect.x) <= 1e6 && std::abs(rect.y) <= 1e6 && rect.width <= 1e6 &&
           rect.height <= 1e6;
}

void Check(bool valid, std::size_t index, const char *detail)
{
    if (!valid) {
        throw std::invalid_argument("DisplayList command " + std::to_string(index) + ": " + detail);
    }
}
} // namespace

void ValidateDisplayList(const DisplayList &list)
{
    if (!list.window) {
        throw std::invalid_argument("DisplayList window must be nonzero");
    }
    if (list.commands.size() > 100000) {
        throw std::invalid_argument("DisplayList command limit is 100000");
    }

    enum class Scope { Clip, Transform, Opacity };
    std::vector<Scope> stack;
    for (std::size_t index = 0; index < list.commands.size(); ++index) {
        const auto &command = list.commands[index];
        if (const auto *rect = std::get_if<FillRect>(&command)) {
            Check(ValidRect(rect->bounds), index, "invalid rectangle bounds");
        } else if (const auto *rect = std::get_if<FillRoundedRect>(&command)) {
            Check(ValidRect(rect->bounds) && std::isfinite(rect->radius) && rect->radius >= 0,
                  index, "invalid rounded rectangle bounds/radius");
        } else if (const auto *rect = std::get_if<StrokeRoundedRect>(&command)) {
            Check(ValidRect(rect->bounds) && std::isfinite(rect->radius) && rect->radius >= 0 &&
                      std::isfinite(rect->width) && rect->width >= 0 && rect->width <= 512,
                  index, "invalid rounded stroke bounds/radius/width");
        } else if (const auto *shadow = std::get_if<RoundedRectShadow>(&command)) {
            Check(ValidRect(shadow->bounds) && std::isfinite(shadow->radius) &&
                      shadow->radius >= 0 && std::isfinite(shadow->blur) && shadow->blur >= 0 &&
                      shadow->blur <= 512 && std::isfinite(shadow->offset_y) &&
                      std::abs(shadow->offset_y) <= 16384,
                  index, "invalid shadow bounds/radius/blur/offset");
        } else if (const auto *icon = std::get_if<DrawIcon>(&command)) {
            Check(ValidRect(icon->bounds) && icon->icon >= VectorIcon::Grid &&
                      icon->icon <= VectorIcon::HeartOutline,
                  index, "invalid icon bounds/type");
        } else if (const auto *image = std::get_if<DrawImage>(&command)) {
            Check(ValidRect(image->destination) && image->fit >= ImageFit::Fill &&
                      image->fit <= ImageFit::Cover,
                  index, "invalid image destination/fit");
        } else if (const auto *run = std::get_if<DrawGlyphRun>(&command)) {
            Check(run->glyphs.size() <= 100000 && std::isfinite(run->font_size) &&
                      run->font_size > 0 && run->font_size <= 512,
                  index, "invalid glyph count/font size");
            for (const auto &glyph : run->glyphs) {
                Check(glyph.glyph_index <= UINT16_MAX && std::isfinite(glyph.origin.x) &&
                          std::isfinite(glyph.origin.y),
                      index, "invalid glyph index/origin");
            }
        } else if (const auto *clip = std::get_if<PushClipRect>(&command)) {
            Check(ValidRect(clip->bounds) && stack.size() < 256, index,
                  "invalid rectangle clip bounds/depth");
            stack.push_back(Scope::Clip);
        } else if (const auto *clip = std::get_if<PushClipRoundedRect>(&command)) {
            Check(ValidRect(clip->bounds) && std::isfinite(clip->radius) && clip->radius >= 0 &&
                      stack.size() < 256,
                  index, "invalid rounded clip bounds/radius/depth");
            stack.push_back(Scope::Clip);
        } else if (const auto *transform = std::get_if<PushTransform>(&command)) {
            Check(stack.size() < 256, index, "transform stack depth exceeds 256");
            for (double value : transform->values) {
                Check(std::isfinite(value) && std::abs(value) <= 1e6, index,
                      "invalid affine transform value");
            }
            stack.push_back(Scope::Transform);
        } else if (const auto *opacity = std::get_if<PushOpacity>(&command)) {
            Check(stack.size() < 256 && std::isfinite(opacity->opacity) && opacity->opacity >= 0 &&
                      opacity->opacity <= 1,
                  index, "invalid group opacity/depth");
            stack.push_back(Scope::Opacity);
        } else if (std::holds_alternative<PopClip>(command)) {
            Check(!stack.empty() && stack.back() == Scope::Clip, index,
                  "clip pop does not match push");
            stack.pop_back();
        } else if (std::holds_alternative<PopTransform>(command)) {
            Check(!stack.empty() && stack.back() == Scope::Transform, index,
                  "transform pop does not match push");
            stack.pop_back();
        } else if (std::holds_alternative<PopOpacity>(command)) {
            Check(!stack.empty() && stack.back() == Scope::Opacity, index,
                  "opacity pop does not match push");
            stack.pop_back();
        }
    }
    if (!stack.empty()) {
        throw std::invalid_argument("DisplayList has an unclosed clip/transform/opacity stack");
    }
}
} // namespace prism::contracts
