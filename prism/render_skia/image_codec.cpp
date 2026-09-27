#include "prism/render_skia/raster_renderer.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <png.h>
#include <sys/stat.h>
#include <unistd.h>

namespace prism::render_skia {
namespace {
struct InputFd {
    int value;

    ~InputFd()
    {
        if (value >= 0) {
            close(value);
        }
    }
};

std::uint32_t BigEndian(const unsigned char *bytes)
{
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
           (std::uint32_t(bytes[2]) << 8) | bytes[3];
}
} // namespace

std::optional<runtime::ImageDescription> RasterRenderer::InspectPng(const std::string &path)
{
    InputFd fd{open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
    struct stat info{};
    if (fd.value < 0 || fstat(fd.value, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 33 || info.st_size > 16 * 1024 * 1024) {
        return {};
    }
    std::array<unsigned char, 33> header{};
    std::size_t offset = 0;
    while (offset < header.size()) {
        const auto bytes = read(fd.value, header.data() + offset, header.size() - offset);
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes <= 0) {
            return {};
        }
        offset += bytes;
    }
    constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (!std::equal(signature.begin(), signature.end(), header.begin()) ||
        BigEndian(header.data() + 8) != 13 || BigEndian(header.data() + 12) != 0x49484452) {
        return {};
    }
    const auto width = BigEndian(header.data() + 16);
    const auto height = BigEndian(header.data() + 20);
    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        return {};
    }
    return runtime::ImageDescription{width, height, static_cast<std::size_t>(width) * height * 4};
}

std::optional<runtime::DecodedImage> RasterRenderer::DecodePng(const std::string &path)
{
    return DecodePngBounded(path, 64 * 1024 * 1024);
}

std::optional<runtime::DecodedImage> RasterRenderer::DecodePngBounded(const std::string &path,
                                                                      std::size_t max_bytes)
{
    // Recheck the fixed metadata before entering libpng. Package resources are
    // immutable while an instance runs; changed dimensions cannot widen its lease.
    const auto description = InspectPng(path);
    if (!description || description->decoded_bytes > max_bytes) {
        return {};
    }
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&png, path.c_str())) {
        png_image_free(&png);
        return {};
    }
    if (png.width != description->width || png.height != description->height) {
        png_image_free(&png);
        return {};
    }
    png.format = PNG_FORMAT_RGBA;
    runtime::DecodedImage decoded;
    decoded.width = png.width;
    decoded.height = png.height;
    try {
        decoded.rgba.resize(description->decoded_bytes);
    } catch (...) {
        png_image_free(&png);
        throw;
    }
    if (!png_image_finish_read(&png, nullptr, decoded.rgba.data(), 0, nullptr)) {
        png_image_free(&png);
        return {};
    }
    png_image_free(&png);
    return decoded;
}
} // namespace prism::render_skia
