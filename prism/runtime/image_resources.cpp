#include "prism/runtime/image_resources.hpp"
#include "prism/runtime/task_scheduler.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
constexpr std::size_t kDecodeLimit = 64 * 1024 * 1024;
constexpr std::size_t kDecoderScratch = 16 * 1024 * 1024;

struct DescriptionOutput final : TaskOutput {
    std::optional<ImageDescription> description;

    std::uint64_t RetainedBytes() const noexcept override
    {
        return sizeof(DescriptionOutput);
    }
};

struct ImageOutput final : TaskOutput {
    std::optional<DecodedImage> pixels;

    std::uint64_t RetainedBytes() const noexcept override
    {
        return sizeof(ImageOutput) + (pixels ? pixels->rgba.capacity() : 0);
    }
};

struct InspectWork {
    ImageResources::Inspector inspector;
    std::string uri;

    std::shared_ptr<const TaskOutput> operator()(std::stop_token stop) const
    {
        auto output = std::make_shared<DescriptionOutput>();
        if (!stop.stop_requested()) {
            output->description = inspector(uri);
        }
        return output;
    }
};

struct DecodeWork {
    ImageResources::BoundedDecoder decoder;
    std::string uri;
    std::size_t limit;

    std::shared_ptr<const TaskOutput> operator()(std::stop_token stop) const
    {
        auto output = std::make_shared<ImageOutput>();
        if (!stop.stop_requested()) {
            output->pixels = decoder(uri, limit);
        }
        return output;
    }
};

struct LegacyDecode {
    ImageResources::Decoder decoder;

    std::optional<DecodedImage> operator()(const std::string &uri, std::size_t) const
    {
        return decoder(uri);
    }
};

ImageResources::BoundedDecoder LegacyAdapter(ImageResources::Decoder decoder)
{
    if (!decoder) {
        throw std::invalid_argument("ImageResources requires a decoder");
    }
    return LegacyDecode{std::move(decoder)};
}
} // namespace

struct ImageResources::Shared {
    Inspector inspector;
    BoundedDecoder decoder;
    std::shared_ptr<TaskScheduler> scheduler;
    std::shared_ptr<TaskChannel> channel;
};

ImageResources::ImageResources(Decoder decoder, std::size_t max_bytes)
    : ImageResources({}, LegacyAdapter(std::move(decoder)), {}, max_bytes)
{
}

ImageResources::ImageResources(Inspector inspector, BoundedDecoder decoder,
                               std::shared_ptr<TaskScheduler> scheduler, std::size_t max_bytes)
    : shared_(std::make_unique<Shared>()), max_bytes_(max_bytes)
{
    if (!decoder || max_bytes == 0) {
        throw std::invalid_argument("ImageResources requires decoder and budget");
    }
    shared_->inspector = std::move(inspector);
    shared_->decoder = std::move(decoder);
    shared_->scheduler = scheduler ? std::move(scheduler) : std::make_shared<TaskScheduler>();
    shared_->channel = shared_->scheduler->OpenChannel();
}

ImageResources::~ImageResources()
{
    shared_->channel->Stop();
}

void ImageResources::Failed(contracts::ResourceId id)
{
    auto &entry = entries_.at(id.value);
    pending_bytes_ -= entry.reserved;
    entry.reserved = 0;
    entry.task = 0;
    entry.state = ImageState::Failed;
    immediate_.push_back({id, ImageState::Failed, {}});
}

void ImageResources::Dispatch(contracts::ResourceId id)
{
    auto &entry = entries_.at(id.value);
    if (entry.state != ImageState::Loading || entry.task) {
        return;
    }
    if (entry.inspecting) {
        const auto task = id.value * 2;
        const auto result = shared_->channel->Submit(
            {task, TaskPriority::Resource, 8192, InspectWork{shared_->inspector, entry.uri}});
        if (result == TaskSubmitResult::Accepted) {
            entry.task = task;
        } else if (result != TaskSubmitResult::Busy) {
            Failed(id);
        }
        return;
    }
    const auto bytes = entry.description->decoded_bytes;
    if (bytes == 0 || bytes > kDecodeLimit || decoded_bytes_ + pending_bytes_ > max_bytes_ ||
        bytes > max_bytes_ - decoded_bytes_ - pending_bytes_) {
        Failed(id);
        return;
    }
    const auto task = id.value * 2 + 1;
    const auto result = shared_->channel->Submit({task, TaskPriority::Resource,
                                                  bytes + kDecoderScratch + sizeof(ImageOutput),
                                                  DecodeWork{shared_->decoder, entry.uri, bytes}});
    if (result == TaskSubmitResult::Accepted) {
        entry.task = task;
        entry.reserved = bytes;
        pending_bytes_ += bytes;
    } else if (result != TaskSubmitResult::Busy) {
        Failed(id);
    }
}

void ImageResources::QueueDecode(contracts::ResourceId id, const ImageDescription &description)
{
    auto &entry = entries_.at(id.value);
    entry.task = 0;
    entry.inspecting = false;
    entry.description = description;
    if (shared_->inspector &&
        (description.width == 0 || description.height == 0 || description.width > 4096 ||
         description.height > 4096 ||
         description.decoded_bytes !=
             static_cast<std::size_t>(description.width) * description.height * 4)) {
        Failed(id);
        return;
    }
    Dispatch(id);
}

contracts::ResourceId ImageResources::Request(std::string uri)
{
    if (auto it = by_uri_.find(uri); it != by_uri_.end()) {
        return it->second;
    }
    const auto pending = std::count_if(entries_.begin(), entries_.end(), [](const auto &item) {
        return item.second.state == ImageState::Loading;
    });
    if (uri.empty() || uri.size() > 4096 || entries_.size() >= 4096 || pending >= 128 ||
        next_id_ >= std::numeric_limits<std::uint64_t>::max() / 2) {
        return {};
    }

    contracts::ResourceId id{next_id_++};
    Entry entry;
    entry.uri = std::move(uri);
    entries_.emplace(id.value, std::move(entry));
    by_uri_.emplace(entries_.at(id.value).uri, id);
    auto &stored = entries_.at(id.value);
    if (shared_->inspector) {
        stored.inspecting = true;
        Dispatch(id);
    } else {
        // Adapters without a size inspector reserve their declared maximum
        // before entering the decoder. Production PNGs always use inspection.
        QueueDecode(id, {0, 0, std::min(kDecodeLimit, max_bytes_)});
    }
    return id;
}

std::vector<ImageUpdate> ImageResources::Poll()
{
    while (auto result = shared_->channel->TakeCompletion()) {
        const contracts::ResourceId id{result->id / 2};
        auto found = entries_.find(id.value);
        if (found == entries_.end() || found->second.task != result->id) {
            continue;
        }
        auto &entry = found->second;
        if (result->error) {
            Failed(id);
            continue;
        }
        if (entry.inspecting) {
            const auto output = std::dynamic_pointer_cast<const DescriptionOutput>(result->output);
            if (!output || !output->description) {
                Failed(id);
            } else {
                QueueDecode(id, *output->description);
            }
            continue;
        }

        const auto output = std::dynamic_pointer_cast<const ImageOutput>(result->output);
        const auto *pixels = output && output->pixels ? &*output->pixels : nullptr;
        if (!pixels || pixels->width == 0 || pixels->height == 0 || pixels->width > 4096 ||
            pixels->height > 4096 ||
            pixels->rgba.size() != static_cast<std::size_t>(pixels->width) * pixels->height * 4 ||
            pixels->rgba.size() > entry.reserved) {
            Failed(id);
            continue;
        }

        pending_bytes_ -= entry.reserved;
        entry.reserved = 0;
        entry.task = 0;
        entry.state = ImageState::Ready;
        decoded_bytes_ += pixels->rgba.size();
        entry.output = result->output;
        immediate_.push_back(
            {id,
             ImageState::Ready,
             {static_cast<double>(pixels->width), static_cast<double>(pixels->height)}});
    }
    for (const auto &[value, entry] : entries_) {
        if (entry.state == ImageState::Loading && !entry.task) {
            Dispatch({value});
        }
    }
    std::vector<ImageUpdate> updates;
    updates.swap(immediate_);
    return updates;
}

int ImageResources::CompletionFd() const noexcept
{
    return shared_->channel->Fd();
}

const DecodedImage *ImageResources::Get(contracts::ResourceId id) const
{
    const auto found = entries_.find(id.value);
    if (found == entries_.end() || found->second.state != ImageState::Ready) {
        return nullptr;
    }
    const auto output = std::dynamic_pointer_cast<const ImageOutput>(found->second.output);
    return output && output->pixels ? &*output->pixels : nullptr;
}

ImageState ImageResources::State(contracts::ResourceId id) const
{
    const auto found = entries_.find(id.value);
    return found == entries_.end() ? ImageState::Failed : found->second.state;
}

std::shared_ptr<const void> ImageResources::Retain(contracts::ResourceId id) const
{
    const auto found = entries_.find(id.value);
    return found == entries_.end() ? nullptr : found->second.output;
}

void ImageResources::Release(contracts::ResourceId id)
{
    const auto found = entries_.find(id.value);
    if (found == entries_.end()) {
        return;
    }
    if (found->second.task) {
        shared_->channel->Cancel(found->second.task);
    }
    if (const auto *pixels = Get(id)) {
        decoded_bytes_ -= pixels->rgba.size();
    }
    pending_bytes_ -= found->second.reserved;
    std::erase_if(immediate_, [&](const ImageUpdate &update) { return update.id == id; });
    by_uri_.erase(found->second.uri);
    entries_.erase(found);
}
} // namespace prism::runtime
