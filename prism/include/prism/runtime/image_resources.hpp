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
class TaskScheduler;
class TaskChannel;
class TaskOutput;

struct ImageDescription {
    std::uint32_t width{}, height{};
    std::size_t decoded_bytes{};
};

// Straight-alpha RGBA pixels. The decoder is supplied by a backend adapter;
// Scene and the resource queue do not depend on Skia or an image codec.
struct DecodedImage {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> rgba;
};

// A decoded image and its scheduler budget reservation share one lifetime.
using ImageLease = std::shared_ptr<const DecodedImage>;

enum class ImageState { Loading, Ready, Failed };

struct ImageUpdate {
    contracts::ResourceId id{};
    ImageState state{ImageState::Failed};
    contracts::LogicalSize intrinsic_size{};
};

class ImageResources {
public:
    using Decoder = std::function<std::optional<DecodedImage>(const std::string &)>;
    using Inspector = std::function<std::optional<ImageDescription>(const std::string &)>;
    using BoundedDecoder =
        std::function<std::optional<DecodedImage>(const std::string &, std::size_t)>;
    explicit ImageResources(Decoder decoder, std::size_t max_bytes = 128 * 1024 * 1024);
    ImageResources(Inspector inspector, BoundedDecoder decoder,
                   std::shared_ptr<TaskScheduler> scheduler,
                   std::size_t max_bytes = 128 * 1024 * 1024);
    ~ImageResources();
    ImageResources(const ImageResources &) = delete;
    ImageResources &operator=(const ImageResources &) = delete;

    // Request, Poll, Get, and State belong to one runtime thread. Only the
    // supplied inspector/decoder runs on the shared scheduler's workers.
    contracts::ResourceId Request(std::string uri);
    std::vector<ImageUpdate> Poll();
    // Borrowed, nonblocking result/capacity-progress notification. Poll may
    // return no final update while retrying a pending request. Callers must
    // not read or close this descriptor.
    int CompletionFd() const noexcept;
    const DecodedImage *Get(contracts::ResourceId id) const;
    ImageState State(contracts::ResourceId id) const;
    ImageLease Retain(contracts::ResourceId id) const;
    std::uint64_t Generation(contracts::ResourceId id) const noexcept;
    void Release(contracts::ResourceId id);

    std::size_t DecodedBytes() const
    {
        return decoded_bytes_;
    }

private:
    struct Shared;

    struct Entry {
        ImageState state{ImageState::Loading};
        std::uint64_t generation{};
        std::string uri;
        std::shared_ptr<const TaskOutput> output;
        std::uint64_t task{};
        std::size_t reserved{};
        bool inspecting{};
        std::optional<ImageDescription> description;
    };

    std::unique_ptr<Shared> shared_;
    std::map<std::uint64_t, Entry> entries_;
    std::map<std::string, contracts::ResourceId, std::less<>> by_uri_;
    std::size_t max_bytes_;
    std::size_t decoded_bytes_{0};
    std::uint64_t next_id_{1};
    std::uint64_t next_generation_{1};
    std::size_t pending_bytes_{};
    std::vector<ImageUpdate> immediate_;
    void QueueDecode(contracts::ResourceId id, const ImageDescription &description);
    void Failed(contracts::ResourceId id);
    void Dispatch(contracts::ResourceId id);
};

} // namespace prism::runtime
