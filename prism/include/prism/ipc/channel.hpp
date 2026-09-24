#pragma once

#include "prism/ipc/ring_buffer.hpp"
#include "prism/core/noncopyable.hpp"
#include <string>
#include <memory>

namespace prism::ipc {

enum class ChannelRole {
    Host,   // WM (Creates and manages lifetime of shm segment)
    Client  // App Backend (Attaches to existing shm segment)
};

/**
 * @brief Facade Pattern: Unifies POSIX shared memory ring buffer and eventfd signaling
 */
class Channel : public core::NonCopyable {
public:
    static std::shared_ptr<Channel> CreateHost(const std::string& name);
    static std::shared_ptr<Channel> ConnectClient(const std::string& name);

    ~Channel();

    // App Backend methods
    bool PushStateDiff(const StateDiffPacket& pkt);
    bool PopEvent(EventPacket& pkt);

    // WM Host methods
    bool PopStateDiff(StateDiffPacket& pkt);
    bool PushEvent(const EventPacket& pkt);

    void NotifyPeer();
    bool WaitForEvent(int timeout_ms = -1);

    int GetEventFd() const { return event_fd_; }
    const std::string& GetName() const { return name_; }
    ShmChannelLayout* GetLayout() { return layout_; }

private:
    Channel(std::string name, ChannelRole role, int shm_fd, int event_fd, ShmChannelLayout* layout);

    std::string name_;
    ChannelRole role_;
    int shm_fd_{-1};
    int event_fd_{-1};
    ShmChannelLayout* layout_{nullptr};
};

} // namespace prism::ipc
