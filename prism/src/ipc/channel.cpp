#include "prism/ipc/channel.hpp"
#include "prism/core/logging.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace prism::ipc {

Channel::Channel(std::string name, ChannelRole role, int shm_fd, int event_fd,
                 ShmChannelLayout *layout)
    : name_(std::move(name)), role_(role), shm_fd_(shm_fd), event_fd_(event_fd), layout_(layout)
{
}

Channel::~Channel()
{
    if (layout_) {
        munmap(layout_, sizeof(ShmChannelLayout));
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
    }
    if (event_fd_ >= 0) {
        close(event_fd_);
    }
    if (role_ == ChannelRole::Host) {
        shm_unlink(name_.c_str());
    }
}

std::shared_ptr<Channel> Channel::CreateHost(const std::string &name)
{
    shm_unlink(name.c_str());

    int shm_fd = shm_open(name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
    if (shm_fd < 0) {
        PRISM_LOG_ERROR("IPC", "Failed to shm_open(create): %s (errno: %d)", strerror(errno),
                        errno);
        return nullptr;
    }

    if (ftruncate(shm_fd, sizeof(ShmChannelLayout)) < 0) {
        PRISM_LOG_ERROR("IPC", "Failed to ftruncate shm: %s", strerror(errno));
        close(shm_fd);
        shm_unlink(name.c_str());
        return nullptr;
    }

    void *ptr =
        mmap(nullptr, sizeof(ShmChannelLayout), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        PRISM_LOG_ERROR("IPC", "Failed to mmap shm: %s", strerror(errno));
        close(shm_fd);
        shm_unlink(name.c_str());
        return nullptr;
    }

    auto *layout = new (ptr) ShmChannelLayout();
    layout->header.magic = SHM_MAGIC;
    layout->header.version = PROTOCOL_VERSION;
    layout->header.wm_pid = static_cast<uint32_t>(getpid());
    layout->header.is_connected.store(false);
    layout->header.is_master_ready.store(false);

    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    PRISM_LOG_INFO("IPC", "Channel Host created: %s (Layout size: %zu KB)", name.c_str(),
                   sizeof(ShmChannelLayout) / 1024);
    return std::shared_ptr<Channel>(new Channel(name, ChannelRole::Host, shm_fd, efd, layout));
}

std::shared_ptr<Channel> Channel::ConnectClient(const std::string &name)
{
    int shm_fd = shm_open(name.c_str(), O_RDWR, 0666);
    if (shm_fd < 0) {
        PRISM_LOG_ERROR("IPC", "Failed to shm_open(connect): %s (errno: %d)", strerror(errno),
                        errno);
        return nullptr;
    }

    void *ptr =
        mmap(nullptr, sizeof(ShmChannelLayout), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        PRISM_LOG_ERROR("IPC", "Failed to mmap client: %s", strerror(errno));
        close(shm_fd);
        return nullptr;
    }

    auto *layout = static_cast<ShmChannelLayout *>(ptr);
    if (layout->header.magic != SHM_MAGIC) {
        PRISM_LOG_ERROR("IPC", "SHM magic mismatch! Expected: 0x%X, Got: 0x%X", SHM_MAGIC,
                        layout->header.magic);
        munmap(ptr, sizeof(ShmChannelLayout));
        close(shm_fd);
        return nullptr;
    }

    layout->header.client_pid = static_cast<uint32_t>(getpid());
    layout->header.is_connected.store(true);

    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    PRISM_LOG_INFO("IPC", "Channel Client attached: %s", name.c_str());
    return std::shared_ptr<Channel>(new Channel(name, ChannelRole::Client, shm_fd, efd, layout));
}

bool Channel::PushStateDiff(const StateDiffPacket &pkt)
{
    return layout_ && layout_->state_ring.Push(pkt);
}

bool Channel::PopEvent(EventPacket &pkt)
{
    return layout_ && layout_->event_ring.Pop(pkt);
}

bool Channel::PopStateDiff(StateDiffPacket &pkt)
{
    return layout_ && layout_->state_ring.Pop(pkt);
}

bool Channel::PushEvent(const EventPacket &pkt)
{
    return layout_ && layout_->event_ring.Push(pkt);
}

void Channel::NotifyPeer()
{
    if (event_fd_ >= 0) {
        uint64_t val = 1;
        ssize_t ret = write(event_fd_, &val, sizeof(val));
        (void)ret;
    }
}

bool Channel::WaitForEvent(int timeout_ms)
{
    if (event_fd_ < 0) {
        return false;
    }
    struct pollfd pfd{};
    pfd.fd = event_fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret > 0 && (pfd.revents & POLLIN)) {
        uint64_t val;
        ssize_t r = read(event_fd_, &val, sizeof(val));
        (void)r;
        return true;
    }
    return false;
}

} // namespace prism::ipc
