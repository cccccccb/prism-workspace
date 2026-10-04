#include "prism/launch/worker_protocol.hpp"
#include <stdexcept>

namespace prism::launch {
namespace {
void Put(std::vector<std::uint8_t> &out, std::uint64_t value, unsigned width)
{
    for (unsigned i = width; i; --i) {
        out.push_back(value >> ((i - 1) * 8));
    }
}

std::uint64_t Get(std::span<const std::uint8_t> in, std::size_t offset, unsigned width)
{
    if (offset > in.size() || width > in.size() - offset) {
        throw std::invalid_argument("Short worker frame");
    }
    std::uint64_t value = 0;
    while (width--) {
        value = (value << 8) | in[offset++];
    }
    return value;
}
} // namespace

std::vector<std::uint8_t> EncodeWorker(const WorkerMessage &message)
{
    std::vector<std::uint8_t> body;
    unsigned type = message.index() + 1;
    if (const auto *ready = std::get_if<WorkerReady>(&message)) {
        Put(body, ready->preparation_ns, 8);
    } else if (const auto *bind = std::get_if<WorkerBind>(&message)) {
        if (!bind->instance.value) {
            throw std::invalid_argument("Zero worker instance ID");
        }
        Put(body, bind->instance.value, 8);
        auto frame = EncodeMessage(bind->request);
        body.insert(body.end(), frame.begin(), frame.end());
        if (bind->deferred_presentation) {
            body.push_back(1);
        }
    } else if (const auto *event = std::get_if<contracts::LaunchEvent>(&message)) {
        body = EncodeMessage(*event);
    } else if (const auto *request = std::get_if<contracts::LaunchRequest>(&message)) {
        body = EncodeMessage(*request);
    } else if (const auto *reply = std::get_if<WorkerReply>(&message)) {
        body = EncodeMessage(reply->event);
    } else if (const auto *cancel = std::get_if<contracts::LaunchCancel>(&message)) {
        body = EncodeMessage(*cancel);
    } else if (const auto *subscribe = std::get_if<contracts::InstanceSubscribe>(&message)) {
        body = EncodeMessage(*subscribe);
    } else if (const auto *update = std::get_if<contracts::InstanceUpdate>(&message)) {
        body = EncodeMessage(*update);
    } else if (const auto *theme = std::get_if<contracts::ThemeSnapshot>(&message)) {
        if (!theme->generation) {
            throw std::invalid_argument("Zero worker theme generation");
        }
        body = EncodeTheme(*theme);
    } else if (const auto *applied = std::get_if<contracts::ThemeApplied>(&message)) {
        body = EncodeThemeApplied(*applied);
    } else if (const auto *request = std::get_if<contracts::ThemeRequest>(&message)) {
        body = EncodeThemeRequest(*request);
    } else if (const auto *event = std::get_if<contracts::ThemeEvent>(&message)) {
        body = EncodeThemeEvent(*event);
    } else if (const auto *subscription = std::get_if<contracts::LayoutSubscription>(&message)) {
        body = contracts::EncodeLayoutSubscription(*subscription);
    } else if (const auto *state = std::get_if<contracts::LayoutStateEvent>(&message)) {
        body = contracts::EncodeLayoutState(*state);
    } else if (const auto *request = std::get_if<contracts::LayoutControlRequest>(&message)) {
        body = contracts::EncodeLayoutControl(*request);
    } else {
        body =
            contracts::EncodeLayoutControlResult(std::get<contracts::LayoutControlResult>(message));
    }
    std::vector<std::uint8_t> out;
    Put(out, 0x50525731, 4);
    Put(out, 1, 2);
    Put(out, type, 2);
    Put(out, body.size(), 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::size_t WorkerFrameSize(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 12) {
        return 0;
    }
    const auto type = Get(bytes, 6, 2), length = Get(bytes, 8, 4);
    if (Get(bytes, 0, 4) != 0x50525731 || Get(bytes, 4, 2) != 1 || type < 1 || type > 16 ||
        length > (type == 14 ? contracts::kMaxLayoutStatePayload : contracts::kMaxThemePayload)) {
        throw std::invalid_argument("Invalid worker header");
    }
    if ((type == 13 && length != 11) || (type == 14 && length < 11) ||
        (type == 15 && length != contracts::kLayoutControlPayload &&
         length != contracts::kWindowControlPayload) ||
        (type == 16 && length != contracts::kLayoutControlResultPayload)) {
        throw std::invalid_argument("Invalid layout worker payload length");
    }
    return 12 + length;
}

WorkerMessage DecodeWorker(std::span<const std::uint8_t> bytes)
{
    const auto size = WorkerFrameSize(bytes);
    if (!size || size != bytes.size()) {
        throw std::invalid_argument("Invalid worker frame length");
    }
    const auto type = Get(bytes, 6, 2);
    auto body = bytes.subspan(12);
    if (type == 1) {
        if (body.size() != 8) {
            throw std::invalid_argument("Invalid ready frame");
        }
        return WorkerReady{Get(body, 0, 8)};
    }
    if (type == 2) {
        const contracts::InstanceId instance{Get(body, 0, 8)};
        if (!instance.value) {
            throw std::invalid_argument("Zero instance ID");
        }
        const auto frame = body.subspan(8);
        const auto length = FrameSize(frame);
        if (!length ||
            (frame.size() != length && (frame.size() != length + 1 || frame.back() != 1))) {
            throw std::invalid_argument("Invalid worker presentation policy");
        }
        return WorkerBind{std::get<contracts::LaunchRequest>(DecodeMessage(frame.first(length))),
                          instance, frame.size() != length};
    }
    if (type == 9) {
        auto theme = contracts::DecodeTheme(body);
        if (!theme.generation) {
            throw std::invalid_argument("Zero worker theme generation");
        }
        return theme;
    }
    if (type == 10) {
        return contracts::DecodeThemeApplied(body);
    }
    if (type == 11) {
        return contracts::DecodeThemeRequest(body);
    }
    if (type == 12) {
        return contracts::DecodeThemeEvent(body);
    }
    if (type == 13) {
        return contracts::DecodeLayoutSubscription(body);
    }
    if (type == 14) {
        return contracts::DecodeLayoutState(body);
    }
    if (type == 15) {
        return contracts::DecodeLayoutControl(body);
    }
    if (type == 16) {
        return contracts::DecodeLayoutControlResult(body);
    }
    auto decoded = DecodeMessage(body);
    if (type == 3) {
        return std::get<contracts::LaunchEvent>(decoded);
    }
    if (type == 4) {
        return std::get<contracts::LaunchRequest>(decoded);
    }
    if (type == 5) {
        return WorkerReply{std::get<contracts::LaunchEvent>(decoded)};
    }
    if (type == 6) {
        return std::get<contracts::LaunchCancel>(decoded);
    }
    if (type == 7) {
        return std::get<contracts::InstanceSubscribe>(decoded);
    }
    return std::get<contracts::InstanceUpdate>(decoded);
}
} // namespace prism::launch
