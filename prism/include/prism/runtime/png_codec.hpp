#pragma once

#include "prism/runtime/image_resources.hpp"
#include <cstddef>
#include <optional>
#include <string>

namespace prism::runtime {

// Pure CPU image preparation. These functions have no Skia, EGL or Wayland
// state and may run in the resource scheduler's worker threads.
std::optional<ImageDescription> InspectPng(const std::string &path);
std::optional<DecodedImage> DecodePng(const std::string &path);
std::optional<DecodedImage> DecodePngBounded(const std::string &path, std::size_t max_bytes);

} // namespace prism::runtime
