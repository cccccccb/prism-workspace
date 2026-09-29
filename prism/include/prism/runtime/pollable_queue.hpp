#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sys/eventfd.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace prism::runtime {

enum class QueuePushResult { Accepted, Replaced, Busy, Closed, SignalError };

// A bounded, nonblocking, single-consumer queue. Multiple producers may push.
// Fd() is for poll/epoll only: callers must not read or close it. Readability
// persists while items are queued, and also after Close() wakes an empty queue.
// Destroy only after producers and the consumer have stopped using the queue.
template <typename T> class PollableQueue {
public:
    using ReplaceTail = bool (*)(const T &, const T &) noexcept;

    explicit PollableQueue(std::size_t capacity) : capacity_(capacity)
    {
        static_assert(std::is_nothrow_move_constructible_v<T>);
        static_assert(std::is_nothrow_move_assignable_v<T>);

        if (capacity == 0) {
            throw std::invalid_argument("PollableQueue capacity must be positive");
        }

        fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "PollableQueue eventfd");
        }
    }

    ~PollableQueue()
    {
        close(fd_);
    }

    PollableQueue(const PollableQueue &) = delete;
    PollableQueue &operator=(const PollableQueue &) = delete;

    int Fd() const noexcept
    {
        return fd_;
    }

    std::size_t Capacity() const noexcept
    {
        return capacity_;
    }

    std::size_t Size() const
    {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    bool IsClosed() const
    {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    // Busy, Closed and SignalError leave item available for retry. Successful
    // Accepted/Replaced results consume it.
    QueuePushResult TryPush(T &&item)
    {
        return Push(item, nullptr);
    }

    // Only the current tail may be replaced. For a render command variant,
    // replace_tail must accept ordinary, superseded Frame commands alone;
    // it must reject milestones and controls. This preserves every barrier
    // and never moves a newer frame ahead of an earlier control command.
    QueuePushResult TryPushLatest(T &&item, ReplaceTail replace_tail)
    {
        return Push(item, replace_tail);
    }

    std::optional<T> TryPop()
    {
        std::lock_guard lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }

        // Drain before removing the last item. An unexpected eventfd error
        // cannot make the caller lose a message that it believes was popped.
        if (items_.size() == 1 && !closed_) {
            Drain();
        }

        std::optional<T> item(std::move(items_.front()));
        items_.pop_front();
        return item;
    }

    // Returns false only if an empty queue could not signal its poller. In
    // that case the queue stays open, so the caller can report/retry closure.
    bool Close()
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return true;
        }

        if (items_.empty() && !Signal()) {
            return false;
        }

        closed_ = true;
        return true;
    }

private:
    QueuePushResult Push(T &item, ReplaceTail replace_tail)
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return QueuePushResult::Closed;
        }

        if (replace_tail && !items_.empty() && replace_tail(items_.back(), item)) {
            items_.back() = std::move(item);
            return QueuePushResult::Replaced;
        }

        if (items_.size() >= capacity_) {
            return QueuePushResult::Busy;
        }

        const bool was_empty = items_.empty();
        items_.push_back(std::move(item));
        if (was_empty && !Signal()) {
            item = std::move(items_.back());
            items_.pop_back();
            return QueuePushResult::SignalError;
        }

        return QueuePushResult::Accepted;
    }

    bool Signal() const noexcept
    {
        constexpr std::uint64_t one = 1;
        ssize_t written;
        do {
            written = write(fd_, &one, sizeof(one));
        } while (written < 0 && errno == EINTR);

        return written == sizeof(one);
    }

    void Drain() const
    {
        std::uint64_t count;
        ssize_t read_bytes;
        do {
            read_bytes = read(fd_, &count, sizeof(count));
        } while (read_bytes < 0 && errno == EINTR);

        if (read_bytes != sizeof(count)) {
            throw std::system_error(read_bytes < 0 ? errno : EIO, std::generic_category(),
                                    "PollableQueue eventfd drain");
        }
    }

    const std::size_t capacity_;
    int fd_{-1};
    mutable std::mutex mutex_;
    std::deque<T> items_;
    bool closed_{};
};

} // namespace prism::runtime
