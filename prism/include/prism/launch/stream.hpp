#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace prism::launch {
// Owns a nonblocking CLOEXEC socket. Bounded IO and frame extraction per turn.
class Stream {
public:
    using Sizer = std::function<std::size_t(std::span<const std::uint8_t>)>;
    explicit Stream(int fd, Sizer sizer);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    int Fd() const { return fd_; }
    bool Closed() const { return closed_; }
    bool WantsWrite() const { return output_offset_ < output_.size(); }
    bool HasPartialFrame() const { return !input_.empty(); }
    bool Queue(std::span<const std::uint8_t> frame);
    void Flush();
    std::vector<std::vector<std::uint8_t>> Receive();
    void Close();
private:
    int fd_;
    Sizer sizer_;
    std::vector<std::uint8_t> input_, output_;
    std::size_t output_offset_{};
    bool closed_{};
};
} // namespace prism::launch
