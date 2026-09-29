#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

namespace prism::runtime {

enum class TerminalReason : std::uint8_t {
    None,
    EventQueueFailure,
    CommandQueueFailure,
    PlatformFailure,
    RenderFailure,
    ResourceFailure,
    OpenFailure,
    InternalFailure,
};

// A one-shot failure latch and an independent stop request. Neither depends on
// space in a frame or event queue. Fd() is for poll/epoll only: never read or
// close it. Readability remains set after either transition. Destroy this
// object only after all producers and the waiter have stopped using it.
class TerminalSignal {
public:
    TerminalSignal()
    {
        fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(), "TerminalSignal eventfd");
        }
    }

    ~TerminalSignal()
    {
        close(fd_);
    }

    TerminalSignal(const TerminalSignal &) = delete;
    TerminalSignal &operator=(const TerminalSignal &) = delete;

    // The first non-None failure is retained. Stop requests do not hide a
    // later cleanup failure, and a later failure cannot replace the first.
    bool Fail(TerminalReason reason) noexcept
    {
        if (reason == TerminalReason::None) {
            return false;
        }

        auto expected = TerminalReason::None;
        if (!reason_.compare_exchange_strong(expected, reason, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return false;
        }

        Signal();
        return true;
    }

    // Close can be requested even when the ordinary command queue is full.
    // It is independent of failure so cleanup can still publish its reason.
    bool RequestStop() noexcept
    {
        if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }

        Signal();
        return true;
    }

    TerminalReason Reason() const noexcept
    {
        return reason_.load(std::memory_order_acquire);
    }

    bool StopRequested() const noexcept
    {
        return stop_requested_.load(std::memory_order_acquire);
    }

    bool NotificationFailed() const noexcept
    {
        return notification_failed_.load(std::memory_order_acquire);
    }

    int Fd() const noexcept
    {
        return fd_;
    }

private:
    void Signal() noexcept
    {
        constexpr std::uint64_t one = 1;
        ssize_t written;
        do {
            written = write(fd_, &one, sizeof(one));
        } while (written < 0 && errno == EINTR);

        // A saturated eventfd is already readable, so EAGAIN cannot lose a
        // wake. Other errors leave the state latched for bounded polling or
        // explicit failure handling by the owner.
        if (written != sizeof(one) && !(written < 0 && errno == EAGAIN)) {
            notification_failed_.store(true, std::memory_order_release);
        }
    }

    int fd_{-1};
    std::atomic<TerminalReason> reason_{TerminalReason::None};
    std::atomic<bool> stop_requested_{};
    std::atomic<bool> notification_failed_{};
};

} // namespace prism::runtime
