#include "prism/launch/protocol.hpp"
#include "prism/launch/package.hpp"
#include <bit>
#include <stdexcept>
#include <string>

namespace prism::launch {
namespace {
using namespace contracts;
constexpr std::uint32_t kMagic = 0x50524c31; // PRL1

void Require(bool valid, const char *reason)
{
    if (!valid) {
        throw std::invalid_argument(reason);
    }
}

void Put(std::vector<std::uint8_t> &bytes, std::uint64_t value, unsigned count)
{
    for (unsigned i = count; i > 0; --i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
    }
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes)
    {
    }

    std::uint64_t Get(unsigned count)
    {
        Require(count <= bytes_.size() - cursor_, "Truncated launch message");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value = (value << 8) | bytes_[cursor_++];
        }
        return value;
    }

    std::string Text()
    {
        const auto count = Get(2);
        Require(count <= bytes_.size() - cursor_, "Truncated launch string");
        const auto start = cursor_;
        cursor_ += count;
        return std::string(reinterpret_cast<const char *>(bytes_.data() + start), count);
    }

    bool Done() const
    {
        return cursor_ == bytes_.size();
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_{};
};

bool Utf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size();) {
        auto first = static_cast<std::uint8_t>(text[i++]);
        if (!first) {
            return false;
        }
        if (first < 0x80) {
            continue;
        }
        unsigned trailing = 0;
        std::uint32_t point = 0, minimum = 0;
        if ((first & 0xe0) == 0xc0) {
            trailing = 1;
            point = first & 0x1f;
            minimum = 0x80;
        } else if ((first & 0xf0) == 0xe0) {
            trailing = 2;
            point = first & 0x0f;
            minimum = 0x800;
        } else if ((first & 0xf8) == 0xf0) {
            trailing = 3;
            point = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (trailing > text.size() - i) {
            return false;
        }
        while (trailing--) {
            auto next = static_cast<std::uint8_t>(text[i++]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            point = (point << 6) | (next & 0x3f);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

void Text(std::vector<std::uint8_t> &bytes, std::string_view text)
{
    Require(text.size() <= 2048 && Utf8(text), "Launch text must be bounded UTF-8 without NUL");
    Put(bytes, text.size(), 2);
    bytes.insert(bytes.end(), text.begin(), text.end());
}

void Check(const LaunchRequest &launch)
{
    Require(launch.request.value && ValidAppId(launch.app_id), "Invalid launch request");
    Require(launch.mode == LaunchMode::ActivateOrCreate || launch.mode == LaunchMode::NewInstance,
            "Unknown launch mode");
}

void Check(const LaunchEvent &event)
{
    Require(event.request.value != 0, "Zero launch request ID");
    Require(static_cast<unsigned>(event.milestone) <=
                static_cast<unsigned>(LaunchMilestone::Activated),
            "Unknown launch milestone");
    Require(static_cast<unsigned>(event.error) <= static_cast<unsigned>(LaunchError::SessionEnded),
            "Unknown launch error");
    Require((event.milestone == LaunchMilestone::Failed) == (event.error != LaunchError::None),
            "Inconsistent launch result");
    Require(event.instance.value != 0 || (event.milestone == LaunchMilestone::Failed && !event.pid),
            "Event needs an instance ID");
    if (event.milestone == LaunchMilestone::Accepted) {
        Require(event.pid == 0, "Accepted event cannot assign a worker");
    } else if (event.milestone != LaunchMilestone::Failed) {
        Require(event.pid != 0, "Event needs a worker PID");
    }
    Require(event.exit_code >= -64 && event.exit_code <= 255 &&
                (event.milestone == LaunchMilestone::Exited || event.exit_code == 0),
            "Invalid process exit code");
    Require(event.detail.size() <= 2048 && Utf8(event.detail), "Invalid launch detail");
}
} // namespace

std::vector<std::uint8_t> EncodeMessage(const LaunchMessage &message)
{
    std::vector<std::uint8_t> payload;
    std::uint16_t type = 1;
    std::uint64_t request = 0, instance = 0;
    if (const auto *launch = std::get_if<contracts::LaunchRequest>(&message)) {
        Check(*launch);
        request = launch->request.value;
        Put(payload, static_cast<std::uint8_t>(launch->mode), 1);
        Text(payload, launch->app_id);
    } else if (const auto *cancel = std::get_if<contracts::LaunchCancel>(&message)) {
        Require(cancel->request.value != 0, "Zero cancellation request ID");
        type = 3;
        request = cancel->request.value;
    } else if (const auto *subscribe = std::get_if<contracts::InstanceSubscribe>(&message)) {
        Require(subscribe->request.value, "Zero subscription ID");
        type = 4;
        request = subscribe->request.value;
    } else if (const auto *update = std::get_if<contracts::InstanceUpdate>(&message)) {
        Require(update->request.value && static_cast<unsigned>(update->change) <= 3,
                "Invalid instance update");
        const bool entry =
            update->change == InstanceChange::Running || update->change == InstanceChange::Stopped;
        Require(entry ? (update->instance.value && update->pid && ValidAppId(update->app_id))
                      : (!update->instance.value && !update->pid && update->app_id.empty()),
                "Invalid instance identity");
        type = 5;
        request = update->request.value;
        instance = update->instance.value;
        Put(payload, update->pid, 4);
        Put(payload, static_cast<unsigned>(update->change), 1);
        Text(payload, update->app_id);
    } else if (const auto *theme = std::get_if<contracts::ThemeRequest>(&message)) {
        type = 6;
        request = theme->request;
        payload = contracts::EncodeThemeRequest(*theme);
    } else if (const auto *theme = std::get_if<contracts::ThemeEvent>(&message)) {
        type = 7;
        request = theme->request;
        payload = contracts::EncodeThemeEvent(*theme);
    } else {
        type = 2;
        const auto &event = std::get<contracts::LaunchEvent>(message);
        Check(event);
        request = event.request.value;
        instance = event.instance.value;
        Put(payload, event.pid, 4);
        Put(payload, static_cast<std::uint8_t>(event.milestone), 1);
        Put(payload, static_cast<std::uint16_t>(event.error), 2);
        Put(payload, std::bit_cast<std::uint32_t>(event.exit_code), 4);
        Text(payload, event.detail);
    }
    std::vector<std::uint8_t> bytes;
    Put(bytes, kMagic, 4);
    Put(bytes, contracts::kLaunchProtocolVersion, 2);
    Put(bytes, type, 2);
    Put(bytes, payload.size(), 4);
    Put(bytes, request, 8);
    Put(bytes, instance, 8);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::size_t FrameSize(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < kLaunchHeaderSize) {
        return 0;
    }
    Reader reader(bytes.first(kLaunchHeaderSize));
    Require(reader.Get(4) == kMagic, "Invalid launch message magic");
    Require(reader.Get(2) == contracts::kLaunchProtocolVersion,
            "Unsupported launch protocol version");
    const auto type = reader.Get(2);
    Require(type >= 1 && type <= 7, "Unknown launch message type");
    const auto length = reader.Get(4);
    Require(length <= contracts::kMaxLaunchPayload, "Launch payload exceeds limit");
    Require(reader.Get(8) != 0, "Zero launch request ID");
    const auto instance = reader.Get(8);
    Require(type == 2 || type == 5 || instance == 0, "Request cannot choose an instance ID");
    return kLaunchHeaderSize + length;
}

LaunchMessage DecodeMessage(std::span<const std::uint8_t> bytes)
{
    const auto length = FrameSize(bytes);
    Require(length && length == bytes.size(), "Expected exactly one complete launch frame");
    Reader header(bytes.first(kLaunchHeaderSize));
    header.Get(4);
    header.Get(2);
    const auto type = header.Get(2);
    header.Get(4);
    const contracts::RequestId request{header.Get(8)};
    const contracts::InstanceId instance{header.Get(8)};
    Reader body(bytes.subspan(kLaunchHeaderSize));
    if (type == 6) {
        auto theme = DecodeThemeRequest(bytes.subspan(kLaunchHeaderSize));
        Require(theme.request == request.value, "Theme request identity mismatch");
        return theme;
    }
    if (type == 7) {
        auto theme = DecodeThemeEvent(bytes.subspan(kLaunchHeaderSize));
        Require(theme.request == request.value, "Theme event identity mismatch");
        return theme;
    }
    if (type == 4) {
        Require(body.Done(), "Subscription cannot contain payload");
        return InstanceSubscribe{request};
    }
    if (type == 5) {
        InstanceUpdate update{request, instance, static_cast<std::uint32_t>(body.Get(4)),
                              static_cast<InstanceChange>(body.Get(1)), body.Text()};
        Require(body.Done(), "Trailing subscription payload");
        EncodeMessage(update);
        return update;
    }
    if (type == 3) {
        Require(body.Done(), "Cancellation cannot contain payload");
        return contracts::LaunchCancel{request};
    }
    if (type == 1) {
        contracts::LaunchRequest launch{
            request, {}, static_cast<contracts::LaunchMode>(body.Get(1))};
        launch.app_id = body.Text();
        Require(body.Done(), "Trailing launch payload");
        Check(launch);
        return launch;
    }
    contracts::LaunchEvent event;
    event.request = request;
    event.instance = instance;
    event.pid = body.Get(4);
    event.milestone = static_cast<contracts::LaunchMilestone>(body.Get(1));
    event.error = static_cast<contracts::LaunchError>(body.Get(2));
    event.exit_code = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(body.Get(4)));
    event.detail = body.Text();
    Require(body.Done(), "Trailing event payload");
    Check(event);
    return event;
}
} // namespace prism::launch
