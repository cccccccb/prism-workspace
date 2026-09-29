#pragma once

#include "include/core/SkImage.h"
#include "include/core/SkTypeface.h"
#include "prism/contracts/types.hpp"
#include "prism/runtime/image_resources.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace prism::render_skia::detail {
// Each renderer owns one table. A shared CPU image lease keeps its decoded
// bytes alive without sharing the table or its mutable generations.
class ResourceTable {
public:
    bool RegisterFont(contracts::ResourceId id, const std::string &path);
    bool RegisterImage(contracts::ResourceId id, const runtime::DecodedImage &image);
    bool RegisterImage(contracts::ResourceId id, runtime::ImageLease image);
    void UnregisterImage(contracts::ResourceId id);

    bool HasFont(contracts::ResourceId id) const noexcept;
    bool HasImage(contracts::ResourceId id) const noexcept;
    const sk_sp<SkTypeface> &Font(contracts::ResourceId id) const;
    const SkImage *Image(contracts::ResourceId id) const noexcept;
    std::uint64_t ImageGeneration(contracts::ResourceId id) const noexcept;
    std::uint64_t ResourceEpoch() const noexcept;

private:
    struct ImageResource {
        sk_sp<SkImage> image;
        std::uint64_t generation{};
    };

    std::unordered_map<std::uint64_t, sk_sp<SkTypeface>> fonts_;
    std::unordered_map<std::uint64_t, ImageResource> images_;
    std::uint64_t resource_epoch_{1};
};
} // namespace prism::render_skia::detail
