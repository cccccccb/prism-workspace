#pragma once

#include "prism/contracts/display_list.hpp"
#include "prism/runtime/scene.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace prism::runtime {

// Owns FreeType faces used to shape scene text into display-list glyph runs.
class TextShaper {
public:
    explicit TextShaper(std::string font_path);
    ~TextShaper();
    TextShaper(const TextShaper &) = delete;
    TextShaper &operator=(const TextShaper &) = delete;

    bool Ready() const;
    bool RegisterFont(contracts::ResourceId id, const std::string &path);
    ShapedText Shape(std::string_view text, double size);
    ShapedText Shape(contracts::ResourceId font, std::string_view text, double size);

    contracts::ResourceId FontId() const
    {
        return default_font_;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    contracts::ResourceId default_font_{1};
};

} // namespace prism::runtime
