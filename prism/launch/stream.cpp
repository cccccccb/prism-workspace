#include "prism/launch/stream.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace prism::launch {
constexpr std::size_t kBufferLimit = 256 * 1024;
Stream::Stream(int fd, Sizer sizer) : fd_(fd), sizer_(std::move(sizer)) {
    if (fd < 0) throw std::invalid_argument("Invalid stream FD");
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd); fd_ = -1; throw std::runtime_error("Stream flags failed");
    }
}
Stream::~Stream() { Close(); }
void Stream::Close() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1; closed_ = true;
}
bool Stream::Queue(std::span<const std::uint8_t> frame) {
    if (closed_) return false;
    if (output_offset_) {
        output_.erase(output_.begin(), output_.begin() + output_offset_);
        output_offset_ = 0;
    }
    if (frame.size() > kBufferLimit - output_.size()) { Close(); return false; }
    output_.insert(output_.end(), frame.begin(), frame.end());
    return true;
}
void Stream::Flush() {
    std::size_t budget = 65536;
    while (!closed_ && WantsWrite() && budget) {
        const auto count = std::min(budget, output_.size() - output_offset_);
        const auto n = send(fd_, output_.data() + output_offset_, count, MSG_NOSIGNAL);
        if (n > 0) { output_offset_ += n; budget -= n; }
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { Close(); break; }
    }
    if (!WantsWrite()) { output_.clear(); output_offset_ = 0; }
}
std::vector<std::vector<std::uint8_t>> Stream::Receive() {
    std::vector<std::vector<std::uint8_t>> frames;
    if (closed_) return frames;
    bool eof = false;
    std::uint8_t buffer[8192];
    std::size_t budget = 65536;
    while (budget) {
        const auto n = recv(fd_, buffer, std::min(budget, sizeof(buffer)), 0);
        if (n > 0) {
            if (static_cast<std::size_t>(n) > kBufferLimit - input_.size()) { Close(); return {}; }
            input_.insert(input_.end(), buffer, buffer + n); budget -= n;
        } else if (n == 0) { eof = true; break; }
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        else { Close(); return {}; }
    }
    std::size_t offset = 0;
    try {
        while (offset < input_.size() && frames.size() < 64) {
            const auto remaining = std::span(input_).subspan(offset);
            const auto size = sizer_(remaining);
            if (!size || size > remaining.size()) break;
            if (size > kBufferLimit) throw std::invalid_argument("Frame exceeds buffer");
            frames.emplace_back(remaining.begin(), remaining.begin() + size);
            offset += size;
        }
        input_.erase(input_.begin(), input_.begin() + offset);
    } catch (...) { Close(); throw; }
    if (eof) Close();
    return frames;
}
} // namespace prism::launch
