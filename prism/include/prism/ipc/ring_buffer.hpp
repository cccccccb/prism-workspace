#pragma once

#include "prism/ipc/protocol.hpp"
#include <atomic>
#include <cstddef>

namespace prism::ipc {

template <typename T, size_t Capacity> class ShmRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

public:
    ShmRingBuffer() : head_(0), tail_(0)
    {
    }

    // Writer (Producer): Lock-free wait-free push
    bool Push(const T &item)
    {
        const uint32_t current_tail = tail_.load(std::memory_order_relaxed);
        const uint32_t current_head = head_.load(std::memory_order_acquire);

        if (current_tail - current_head >= Capacity) {
            return false; // Buffer is full
        }

        data_[current_tail & (Capacity - 1)] = item;
        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    // Reader (Consumer): Lock-free wait-free pop
    bool Pop(T &item)
    {
        const uint32_t current_head = head_.load(std::memory_order_relaxed);
        const uint32_t current_tail = tail_.load(std::memory_order_acquire);

        if (current_head == current_tail) {
            return false; // Buffer is empty
        }

        item = data_[current_head & (Capacity - 1)];
        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    bool IsEmpty() const
    {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    size_t Size() const
    {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        return (t >= h) ? (t - h) : 0;
    }

private:
    alignas(64) std::atomic<uint32_t> head_;
    alignas(64) std::atomic<uint32_t> tail_;
    alignas(64) T data_[Capacity];
};

using StateRingBuffer = ShmRingBuffer<StateDiffPacket, RING_BUFFER_CAPACITY>;
using EventRingBuffer = ShmRingBuffer<EventPacket, RING_BUFFER_CAPACITY>;

struct ShmChannelLayout {
    ShmHeader header;
    StateRingBuffer state_ring; // Backend -> WM
    EventRingBuffer event_ring; // WM -> Backend
};

} // namespace prism::ipc
