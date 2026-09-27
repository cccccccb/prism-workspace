#pragma once
#include "prism/contracts/types.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::runtime {

// Straight-alpha RGBA pixels. The decoder is supplied by a backend adapter;
// Scene and the resource queue do not depend on Skia or an image codec.
struct DecodedImage {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> rgba;
};

enum class ImageState { Loading, Ready, Failed };
struct ImageUpdate {
    contracts::ResourceId id{};
    ImageState state{ImageState::Failed};
    contracts::LogicalSize intrinsic_size{};
};

class ImageResources {
public:
    using Decoder = std::function<std::optional<DecodedImage>(const std::string&)>;
    explicit ImageResources(Decoder decoder, std::size_t max_bytes = 128 * 1024 * 1024);
    ~ImageResources();
    ImageResources(const ImageResources&) = delete;
    ImageResources& operator=(const ImageResources&) = delete;

    // Request, Poll, Get, and State belong to one runtime thread. Only the
    // supplied decoder runs on the worker thread.
    contracts::ResourceId Request(std::string uri);
    std::vector<ImageUpdate> Poll();
    // Borrowed, nonblocking completion notification. Poll drains it atomically
    // with the completion queue; callers must not read or close this descriptor.
    int CompletionFd() const noexcept;
    const DecodedImage* Get(contracts::ResourceId id) const;
    ImageState State(contracts::ResourceId id) const;
    std::size_t DecodedBytes() const { return decoded_bytes_; }

private:
    struct Shared;
    struct Entry {
        ImageState state{ImageState::Loading};
        std::optional<DecodedImage> pixels;
    };
    std::unique_ptr<Shared> shared_;
    std::map<std::uint64_t, Entry> entries_;
    std::map<std::string, contracts::ResourceId, std::less<>> by_uri_;
    std::size_t max_bytes_;
    std::size_t decoded_bytes_{0};
    std::uint64_t next_id_{1};
};

} // namespace prism::runtime
