#include "prism/runtime/image_resources.hpp"
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sys/eventfd.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace prism::runtime {

struct ImageResources::Shared {
    struct Job {
        contracts::ResourceId id;
        std::string uri;
    };

    struct Completion {
        contracts::ResourceId id;
        std::optional<DecodedImage> pixels;
    };

    Decoder decoder;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::deque<Completion> completed;
    std::thread worker;
    bool stop{false};
    int completion_fd{-1};

    ~Shared()
    {
        if (completion_fd >= 0) {
            close(completion_fd);
        }
    }
};

ImageResources::ImageResources(Decoder decoder, std::size_t max_bytes)
    : shared_(std::make_unique<Shared>()), max_bytes_(max_bytes)
{
    if (!decoder || max_bytes == 0) {
        throw std::invalid_argument("ImageResources requires decoder and budget");
    }
    shared_->completion_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (shared_->completion_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "Image completion eventfd");
    }
    shared_->decoder = std::move(decoder);
    shared_->worker = std::thread(&ImageResources::RunWorker, this);
}

void ImageResources::RunWorker()
{
    for (;;) {
        Shared::Job job;
        {
            std::unique_lock lock(shared_->mutex);
            // Hold at most one decoded image in the completion queue until
            // the runtime thread applies its memory budget.
            shared_->wake.wait(lock, [this] {
                return shared_->stop || (!shared_->jobs.empty() && shared_->completed.empty());
            });
            if (shared_->stop) {
                return;
            }
            job = std::move(shared_->jobs.front());
            shared_->jobs.pop_front();
        }

        std::optional<DecodedImage> image;
        try {
            image = shared_->decoder(job.uri);
        } catch (...) {
            image.reset();
        }
        {
            std::lock_guard lock(shared_->mutex);
            shared_->completed.push_back({job.id, std::move(image)});
            const std::uint64_t one = 1;
            // The queue is bounded to one completion. EAGAIN already means
            // readable; retry interruption without losing the notification.
            while (write(shared_->completion_fd, &one, sizeof(one)) < 0 && errno == EINTR) {
            }
        }
    }
}

ImageResources::~ImageResources()
{
    {
        std::lock_guard lock(shared_->mutex);
        shared_->stop = true;
    }

    shared_->wake.notify_one();
    if (shared_->worker.joinable()) {
        shared_->worker.join();
    }
}

contracts::ResourceId ImageResources::Request(std::string uri)
{
    if (auto it = by_uri_.find(uri); it != by_uri_.end()) {
        return it->second;
    }
    if (uri.empty() || uri.size() > 4096 || entries_.size() >= 4096 ||
        next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return {};
    }
    contracts::ResourceId id{next_id_++};
    entries_.emplace(id.value, Entry{});
    by_uri_.emplace(uri, id);
    {
        std::lock_guard lock(shared_->mutex);
        shared_->jobs.push_back({id, std::move(uri)});
    }

    shared_->wake.notify_one();
    return id;
}

std::vector<ImageUpdate> ImageResources::Poll()
{
    std::deque<Shared::Completion> completed;
    {
        std::lock_guard lock(shared_->mutex);
        completed.swap(shared_->completed);
        std::uint64_t value;
        while (read(shared_->completion_fd, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
    }

    shared_->wake.notify_one();

    std::vector<ImageUpdate> updates;
    for (auto &result : completed) {
        auto it = entries_.find(result.id.value);
        if (it == entries_.end()) {
            continue;
        }
        auto &entry = it->second;
        const auto &image = result.pixels;
        const std::size_t bytes = image ? image->rgba.size() : 0;
        if (!image || image->width == 0 || image->height == 0 || image->width > 4096 ||
            image->height > 4096 ||
            bytes != static_cast<std::size_t>(image->width) * image->height * 4 ||
            bytes > max_bytes_ - decoded_bytes_) {
            entry.state = ImageState::Failed;
            updates.push_back({result.id, ImageState::Failed, {}});
            continue;
        }
        entry.state = ImageState::Ready;
        decoded_bytes_ += bytes;
        entry.pixels = std::move(result.pixels);
        updates.push_back({result.id,
                           ImageState::Ready,
                           {static_cast<double>(entry.pixels->width),
                            static_cast<double>(entry.pixels->height)}});
    }
    return updates;
}

int ImageResources::CompletionFd() const noexcept
{
    return shared_->completion_fd;
}

const DecodedImage *ImageResources::Get(contracts::ResourceId id) const
{
    auto it = entries_.find(id.value);
    return it != entries_.end() && it->second.state == ImageState::Ready ? &*it->second.pixels
                                                                         : nullptr;
}

ImageState ImageResources::State(contracts::ResourceId id) const
{
    auto it = entries_.find(id.value);
    return it == entries_.end() ? ImageState::Failed : it->second.state;
}

} // namespace prism::runtime
