#pragma once

#include "prism/contracts/types.hpp"

class SkImage;

namespace prism::render_skia::detail {
class ImageProvider {
public:
    virtual ~ImageProvider() = default;
    virtual const SkImage *Find(contracts::ResourceId id) const noexcept = 0;
};
} // namespace prism::render_skia::detail
