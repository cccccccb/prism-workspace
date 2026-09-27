#include "prism/sdk/launch_client.hpp"
#include "prism/launch/stream.hpp"
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace prism::sdk {
std::string DefaultLaunchSocket()
{
    const auto *dir = std::getenv("XDG_RUNTIME_DIR");
    return dir ? std::string(dir) + "/prism/launcher.sock" : std::string{};
}

LaunchClient::LaunchClient(std::string socket)
    : socket_(socket.empty() ? DefaultLaunchSocket() : std::move(socket))
{
}

LaunchClient::~LaunchClient() = default;

bool LaunchClient::Connect()
{
    if (Connected()) {
        return true;
    }
    if (stream_) {
        return false; // Never silently reconnect a lost request stream.
    }
    sockaddr_un address{};
    if (socket_.empty() || socket_.size() >= sizeof(address.sun_path)) {
        return false;
    }
    struct stat info{};
    if (lstat(socket_.c_str(), &info) || !S_ISSOCK(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 0777) != 0600) {
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_.c_str(), socket_.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
        close(fd);
        return false;
    }
    ucred credential{};
    socklen_t length = sizeof(credential);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &length) ||
        credential.uid != geteuid()) {
        close(fd);
        return false;
    }
    stream_ = std::make_unique<launch::Stream>(fd, launch::FrameSize);
    return true;
}

bool LaunchClient::Connected() const
{
    return stream_ && !stream_->Closed();
}

int LaunchClient::Fd() const
{
    return Connected() ? stream_->Fd() : -1;
}

bool LaunchClient::WantsWrite() const
{
    return Connected() && stream_->WantsWrite();
}

bool LaunchClient::HasCompleteFrame() const
{
    return Connected() && stream_->HasCompleteFrame();
}

std::uint64_t LaunchClient::Launch(std::string app_id, contracts::LaunchMode mode)
{
    if (!Connect() || !next_request_) {
        return 0;
    }
    const contracts::RequestId request{next_request_++};
    try {
        const auto frame =
            launch::EncodeMessage(contracts::LaunchRequest{request, std::move(app_id), mode});
        if (!stream_->Queue(frame)) {
            return 0;
        }
        stream_->Flush();
        return Connected() ? request.value : 0;
    } catch (...) {
        return 0;
    }
}

std::uint64_t LaunchClient::SubscribeInstances()
{
    if (!Connect() || !next_request_) {
        return 0;
    }
    auto id = next_request_++;
    if (!stream_->Queue(launch::EncodeMessage(contracts::InstanceSubscribe{{id}}))) {
        return 0;
    }
    stream_->Flush();
    return Connected() ? id : 0;
}

std::vector<contracts::InstanceUpdate> LaunchClient::TakeInstanceUpdates()
{
    auto result = std::move(updates_);
    updates_.clear();
    return result;
}

std::uint64_t LaunchClient::SelectTheme(std::string id, std::string color_scheme)
{
    if (!Connect() || !next_request_) {
        return 0;
    }
    const auto request = next_request_++;
    try {
        if (!stream_->Queue(launch::EncodeMessage(
                contracts::ThemeRequest{request, std::move(id), std::move(color_scheme)}))) {
            return 0;
        }
        stream_->Flush();
        return Connected() ? request : 0;
    } catch (...) {
        return 0;
    }
}

std::vector<contracts::ThemeEvent> LaunchClient::TakeThemeEvents()
{
    auto result = std::move(themes_);
    themes_.clear();
    return result;
}

bool LaunchClient::Cancel(contracts::RequestId request)
{
    if (!Connected()) {
        return false;
    }
    try {
        if (!stream_->Queue(launch::EncodeMessage(contracts::LaunchCancel{request}))) {
            return false;
        }
        stream_->Flush();
        return Connected();
    } catch (...) {
        return false;
    }
}

std::vector<contracts::LaunchEvent> LaunchClient::Pump(int timeout)
{
    std::vector<contracts::LaunchEvent> events;
    if (!Connected()) {
        return events;
    }
    stream_->Flush();
    pollfd fd{stream_->Fd(), static_cast<short>(POLLIN | (stream_->WantsWrite() ? POLLOUT : 0)), 0};
    poll(&fd, 1, stream_->HasCompleteFrame() ? 0 : timeout);
    stream_->Flush();
    for (auto &frame : stream_->Receive()) {
        auto message = launch::DecodeMessage(frame);
        if (const auto *event = std::get_if<contracts::LaunchEvent>(&message)) {
            events.push_back(*event);
        } else if (const auto *update = std::get_if<contracts::InstanceUpdate>(&message)) {
            if (updates_.size() >= 4096) {
                stream_->Close();
                break;
            }
            updates_.push_back(*update);
        } else if (const auto *theme = std::get_if<contracts::ThemeEvent>(&message)) {
            if (themes_.size() >= 256) {
                stream_->Close();
                break;
            }
            themes_.push_back(*theme);
        } else {
            stream_->Close();
            break;
        }
    }
    return events;
}
} // namespace prism::sdk
