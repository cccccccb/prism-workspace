#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "resource_table_p.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace prism::render_skia::detail {
namespace {
bool ValidImage(contracts::ResourceId id, const runtime::DecodedImage &image,
                std::uint64_t resource_epoch)
{
    return id && image.width > 0 && image.height > 0 && image.width <= 4096 &&
           image.height <= 4096 &&
           image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4 &&
           resource_epoch != UINT64_MAX;
}

SkImageInfo ImageInfo(const runtime::DecodedImage &image)
{
    return SkImageInfo::Make(static_cast<int>(image.width), static_cast<int>(image.height),
                             kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
}

void ReleaseImageOwner(const void *, void *context)
{
    delete static_cast<runtime::ImageLease *>(context);
}
} // namespace

bool ResourceTable::RegisterFont(contracts::ResourceId id, const std::string &path)
{
    if (!id || HasFont(id) || resource_epoch_ == UINT64_MAX) {
        return false;
    }

    auto typeface = SkTypeface::MakeFromFile(path.c_str());
    if (!typeface) {
        return false;
    }

    fonts_.emplace(id.value, std::move(typeface));
    ++resource_epoch_;
    return true;
}

bool ResourceTable::RegisterImage(contracts::ResourceId id, const runtime::DecodedImage &image)
{
    if (!ValidImage(id, image, resource_epoch_)) {
        return false;
    }

    const SkPixmap pixmap(ImageInfo(image), image.rgba.data(),
                          static_cast<std::size_t>(image.width) * 4);
    auto sk_image = SkImages::RasterFromPixmapCopy(pixmap);
    if (!sk_image) {
        return false;
    }

    images_.insert_or_assign(id.value, ImageResource{std::move(sk_image), resource_epoch_ + 1});
    ++resource_epoch_;
    return true;
}

bool ResourceTable::RegisterImage(contracts::ResourceId id, runtime::ImageLease image)
{
    if (!image || !ValidImage(id, *image, resource_epoch_)) {
        return false;
    }

    auto keeper = std::make_unique<runtime::ImageLease>(std::move(image));
    const auto &source = **keeper;
    auto data = SkData::MakeWithProc(source.rgba.data(), source.rgba.size(), ReleaseImageOwner,
                                     keeper.get());
    if (!data) {
        return false;
    }
    keeper.release();

    auto sk_image = SkImages::RasterFromData(ImageInfo(source), std::move(data), source.width * 4);
    if (!sk_image) {
        return false;
    }

    images_.insert_or_assign(id.value, ImageResource{std::move(sk_image), resource_epoch_ + 1});
    ++resource_epoch_;
    return true;
}

void ResourceTable::UnregisterImage(contracts::ResourceId id)
{
    if (images_.erase(id.value) && resource_epoch_ != UINT64_MAX) {
        ++resource_epoch_;
    }
}

bool ResourceTable::HasFont(contracts::ResourceId id) const noexcept
{
    return fonts_.contains(id.value);
}

bool ResourceTable::HasImage(contracts::ResourceId id) const noexcept
{
    return images_.contains(id.value);
}

const sk_sp<SkTypeface> &ResourceTable::Font(contracts::ResourceId id) const
{
    return fonts_.at(id.value);
}

const SkImage *ResourceTable::Image(contracts::ResourceId id) const noexcept
{
    const auto found = images_.find(id.value);
    return found == images_.end() ? nullptr : found->second.image.get();
}

std::uint64_t ResourceTable::ImageGeneration(contracts::ResourceId id) const noexcept
{
    const auto found = images_.find(id.value);
    return found == images_.end() ? 0 : found->second.generation;
}

std::uint64_t ResourceTable::ResourceEpoch() const noexcept
{
    return resource_epoch_;
}
} // namespace prism::render_skia::detail
