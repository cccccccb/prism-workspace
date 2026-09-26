#include "prism/launch/worker_protocol.hpp"
#include <stdexcept>
namespace prism::launch {
namespace {
void Put(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned width) {
    for (unsigned i = width; i; --i) out.push_back(value >> ((i - 1) * 8));
}
std::uint64_t Get(std::span<const std::uint8_t> in, std::size_t offset, unsigned width) {
    if (offset > in.size() || width > in.size() - offset) throw std::invalid_argument("Short worker frame");
    std::uint64_t value = 0;
    while (width--) value = (value << 8) | in[offset++];
    return value;
}
}
std::vector<std::uint8_t> EncodeWorker(const WorkerMessage& message) {
    std::vector<std::uint8_t> body;
    unsigned type = message.index() + 1;
    if (const auto* ready = std::get_if<WorkerReady>(&message)) Put(body, ready->preparation_ns, 8);
    else if (const auto* bind = std::get_if<WorkerBind>(&message)) {
        if (!bind->instance.value) throw std::invalid_argument("Zero worker instance ID");
        Put(body, bind->instance.value, 8);
        auto frame = EncodeMessage(bind->request); body.insert(body.end(), frame.begin(), frame.end());
    } else if (const auto* event = std::get_if<contracts::LaunchEvent>(&message)) body = EncodeMessage(*event);
    else if (const auto* request = std::get_if<contracts::LaunchRequest>(&message)) body = EncodeMessage(*request);
    else if (const auto* reply = std::get_if<WorkerReply>(&message)) body = EncodeMessage(reply->event);
    else if (const auto* cancel=std::get_if<contracts::LaunchCancel>(&message)) body=EncodeMessage(*cancel);
    else if (const auto* subscribe=std::get_if<contracts::InstanceSubscribe>(&message)) body=EncodeMessage(*subscribe);
    else body=EncodeMessage(std::get<contracts::InstanceUpdate>(message));
    std::vector<std::uint8_t> out;
    Put(out, 0x50525731, 4); Put(out, 1, 2); Put(out, type, 2); Put(out, body.size(), 4);
    out.insert(out.end(), body.begin(), body.end()); return out;
}
std::size_t WorkerFrameSize(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12) return 0;
    const auto type = Get(bytes, 6, 2), length = Get(bytes, 8, 4);
    if (Get(bytes, 0, 4) != 0x50525731 || Get(bytes, 4, 2) != 1 || type < 1 || type > 8 ||
        length > contracts::kMaxLaunchPayload + kLaunchHeaderSize + 8)
        throw std::invalid_argument("Invalid worker header");
    return 12 + length;
}
WorkerMessage DecodeWorker(std::span<const std::uint8_t> bytes) {
    const auto size = WorkerFrameSize(bytes);
    if (!size || size != bytes.size()) throw std::invalid_argument("Invalid worker frame length");
    const auto type = Get(bytes, 6, 2);
    auto body = bytes.subspan(12);
    if (type == 1) {
        if (body.size() != 8) throw std::invalid_argument("Invalid ready frame");
        return WorkerReady{Get(body, 0, 8)};
    }
    if (type == 2) {
        const contracts::InstanceId instance{Get(body, 0, 8)};
        if (!instance.value) throw std::invalid_argument("Zero instance ID");
        return WorkerBind{std::get<contracts::LaunchRequest>(DecodeMessage(body.subspan(8))), instance};
    }
    auto decoded = DecodeMessage(body);
    if (type == 3) return std::get<contracts::LaunchEvent>(decoded);
    if (type == 4) return std::get<contracts::LaunchRequest>(decoded);
    if (type == 5) return WorkerReply{std::get<contracts::LaunchEvent>(decoded)};
    if (type==6) return std::get<contracts::LaunchCancel>(decoded);
    if (type==7) return std::get<contracts::InstanceSubscribe>(decoded);
    return std::get<contracts::InstanceUpdate>(decoded);
}
} // namespace prism::launch
