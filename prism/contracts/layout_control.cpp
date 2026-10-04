#include "prism/contracts/layout_control.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace prism::contracts {
namespace {
void Require(bool valid)
{
    if (!valid) {
        throw std::invalid_argument("Invalid layout control payload");
    }
}

struct Writer {
    std::vector<std::uint8_t> bytes;

    void U(std::uint64_t value, unsigned size)
    {
        for (unsigned i = size; i; --i) {
            bytes.push_back(value >> ((i - 1) * 8));
        }
    }

    void Point(LogicalPoint point)
    {
        U(std::bit_cast<std::uint64_t>(point.x), 8);
        U(std::bit_cast<std::uint64_t>(point.y), 8);
    }
};

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};

    std::uint64_t U(unsigned size)
    {
        Require(size <= bytes.size() - at);
        std::uint64_t result{};
        while (size--) {
            result = (result << 8) | bytes[at++];
        }
        return result;
    }

    LogicalPoint Point()
    {
        return {std::bit_cast<double>(U(8)), std::bit_cast<double>(U(8))};
    }

    bool Flag()
    {
        const auto value = U(1);
        Require(value <= 1);
        return value;
    }
};

void ValidatePoint(LogicalPoint point)
{
    Require(std::isfinite(point.x) && std::isfinite(point.y) &&
            std::abs(point.x) <= kMaxLayoutControlCoordinate &&
            std::abs(point.y) <= kMaxLayoutControlCoordinate);
}

void ValidateResult(const LayoutControlResult &result)
{
    Require(result.request && result.gesture && result.sequence);
    Require(static_cast<unsigned>(result.status) <=
                static_cast<unsigned>(LayoutControlStatus::Rejected) &&
            static_cast<unsigned>(result.error) <=
                static_cast<unsigned>(LayoutControlError::Disconnected));
    if (result.status == LayoutControlStatus::Began ||
        result.status == LayoutControlStatus::Updated ||
        result.status == LayoutControlStatus::Ended) {
        Require(result.session && result.error == LayoutControlError::None);
    }
    if (result.status == LayoutControlStatus::Rejected) {
        Require(result.error != LayoutControlError::None);
    }
    Require(!result.applied || result.status == LayoutControlStatus::Ended);
    ValidatePoint(result.position);
}

void WriteTarget(Writer &writer, const LayoutControlTarget &target)
{
    writer.U(target.wm_session, 8);
    writer.U(target.output, 8);
    writer.U(target.workspace, 8);
    writer.U(target.root, 8);
    writer.U(target.boundary, 8);
    writer.U(target.topology_revision, 8);
    writer.U(target.layout_revision, 8);
}

LayoutControlTarget ReadTarget(Reader &reader)
{
    return {reader.U(8), reader.U(8), reader.U(8), reader.U(8),
            reader.U(8), reader.U(8), reader.U(8)};
}
} // namespace

bool AllowsLayoutIntent(LayoutControlOperation operation, LayoutControlIntent intent)
{
    if (intent == LayoutControlIntent::None) {
        return true;
    }
    switch (operation) {
    case LayoutControlOperation::GroupGesture:
        return intent == LayoutControlIntent::EnterImmersive ||
               intent == LayoutControlIntent::ExitImmersive;
    case LayoutControlOperation::BoundaryGesture:
        return intent == LayoutControlIntent::ApplyBoundary;
    case LayoutControlOperation::WindowGesture:
        return intent >= LayoutControlIntent::EnterWindowFullscreen &&
               intent <= LayoutControlIntent::SplitVertical;
    }
    return false;
}

void ValidateLayoutControl(const LayoutControlRequest &request)
{
    Require(request.request && request.gesture && request.sequence);
    Require(
        static_cast<unsigned>(request.phase) <= static_cast<unsigned>(LayoutControlPhase::Cancel) &&
        static_cast<unsigned>(request.operation) <=
            static_cast<unsigned>(LayoutControlOperation::WindowGesture) &&
        static_cast<unsigned>(request.intent) <=
            static_cast<unsigned>(LayoutControlIntent::SplitVertical) &&
        static_cast<unsigned>(request.input.kind) <= static_cast<unsigned>(LayoutInputKind::Touch));
    if (request.phase == LayoutControlPhase::Begin) {
        Require(!request.session && request.sequence == 1);
    } else {
        Require(request.session && request.sequence > 1);
    }
    if (request.phase != LayoutControlPhase::End) {
        Require(request.intent == LayoutControlIntent::None);
    }

    const auto &target = request.target;
    Require(target.wm_session && target.output && target.workspace && target.root &&
            target.topology_revision && target.layout_revision);
    const bool boundary = request.operation == LayoutControlOperation::BoundaryGesture;
    Require(boundary ? target.boundary != 0 : target.boundary == 0);
    Require(request.operation == LayoutControlOperation::WindowGesture ? target.node != 0
                                                                       : target.node == 0);
    Require(AllowsLayoutIntent(request.operation, request.intent));
    Require(request.input.contact >= 0 &&
            (request.input.kind == LayoutInputKind::Touch || request.input.contact == 0));
    ValidatePoint(request.position);
}

std::vector<std::uint8_t> EncodeLayoutControl(const LayoutControlRequest &request)
{
    ValidateLayoutControl(request);

    Writer writer;
    writer.U(request.operation == LayoutControlOperation::WindowGesture ? 2 : kLayoutControlVersion,
             2);
    writer.U(request.request, 8);
    writer.U(request.gesture, 8);
    writer.U(request.session, 8);
    writer.U(request.sequence, 8);
    writer.U(static_cast<unsigned>(request.phase), 1);
    writer.U(static_cast<unsigned>(request.operation), 1);
    writer.U(static_cast<unsigned>(request.intent), 1);
    WriteTarget(writer, request.target);
    writer.U(static_cast<unsigned>(request.input.kind), 1);
    writer.U(request.input.serial, 4);
    writer.U(static_cast<std::uint32_t>(request.input.contact), 4);
    writer.Point(request.position);
    if (request.operation == LayoutControlOperation::WindowGesture) {
        writer.U(request.target.node, 8);
    }
    return std::move(writer.bytes);
}

LayoutControlRequest DecodeLayoutControl(std::span<const std::uint8_t> bytes)
{
    Require(bytes.size() == kLayoutControlPayload || bytes.size() == kWindowControlPayload);
    Reader reader{bytes};
    const auto version = reader.U(2);
    Require((version == 1 && bytes.size() == kLayoutControlPayload) ||
            (version == 2 && bytes.size() == kWindowControlPayload));

    LayoutControlRequest request;
    request.request = reader.U(8);
    request.gesture = reader.U(8);
    request.session = reader.U(8);
    request.sequence = reader.U(8);
    request.phase = static_cast<LayoutControlPhase>(reader.U(1));
    request.operation = static_cast<LayoutControlOperation>(reader.U(1));
    request.intent = static_cast<LayoutControlIntent>(reader.U(1));
    request.target = ReadTarget(reader);
    request.input.kind = static_cast<LayoutInputKind>(reader.U(1));
    request.input.serial = reader.U(4);
    request.input.contact = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.U(4)));
    request.position = reader.Point();
    if (version == 2) {
        request.target.node = reader.U(8);
    }
    Require((version == 2) == (request.operation == LayoutControlOperation::WindowGesture));
    ValidateLayoutControl(request);
    return request;
}

std::vector<std::uint8_t> EncodeLayoutControlResult(const LayoutControlResult &result)
{
    ValidateResult(result);

    Writer writer;
    writer.U(kLayoutControlVersion, 2);
    writer.U(result.request, 8);
    writer.U(result.gesture, 8);
    writer.U(result.session, 8);
    writer.U(result.sequence, 8);
    writer.U(static_cast<unsigned>(result.status), 1);
    writer.U(static_cast<unsigned>(result.error), 1);
    writer.U(result.revision, 8);
    writer.U(result.topology_revision, 8);
    writer.U(result.layout_revision, 8);
    writer.Point(result.position);
    writer.U(result.applied, 1);
    return std::move(writer.bytes);
}

LayoutControlResult DecodeLayoutControlResult(std::span<const std::uint8_t> bytes)
{
    Require(bytes.size() == kLayoutControlResultPayload);
    Reader reader{bytes};
    Require(reader.U(2) == kLayoutControlVersion);

    LayoutControlResult result;
    result.request = reader.U(8);
    result.gesture = reader.U(8);
    result.session = reader.U(8);
    result.sequence = reader.U(8);
    result.status = static_cast<LayoutControlStatus>(reader.U(1));
    result.error = static_cast<LayoutControlError>(reader.U(1));
    result.revision = reader.U(8);
    result.topology_revision = reader.U(8);
    result.layout_revision = reader.U(8);
    result.position = reader.Point();
    result.applied = reader.Flag();
    ValidateResult(result);
    return result;
}

std::vector<std::uint8_t> EncodeLayoutSubscription(const LayoutSubscription &subscription)
{
    Require(subscription.request);
    Writer writer;
    writer.U(kLayoutControlVersion, 2);
    writer.U(subscription.request, 8);
    writer.U(subscription.enabled, 1);
    return std::move(writer.bytes);
}

LayoutSubscription DecodeLayoutSubscription(std::span<const std::uint8_t> bytes)
{
    Require(bytes.size() == 11);
    Reader reader{bytes};
    Require(reader.U(2) == kLayoutControlVersion);
    LayoutSubscription result{reader.U(8), reader.Flag()};
    Require(result.request);
    return result;
}

std::vector<std::uint8_t> EncodeLayoutState(const LayoutStateEvent &event)
{
    Require(event.subscription && static_cast<unsigned>(event.status) <=
                                      static_cast<unsigned>(LayoutStateStatus::Disconnected));
    Writer writer;
    writer.U(kLayoutControlVersion, 2);
    writer.U(event.subscription, 8);
    writer.U(static_cast<unsigned>(event.status), 1);
    if (event.status == LayoutStateStatus::Current) {
        const auto snapshot = EncodeLayoutSnapshot(event.snapshot);
        writer.bytes.insert(writer.bytes.end(), snapshot.begin(), snapshot.end());
    } else {
        Require(event.snapshot == LayoutSnapshot{});
    }
    return std::move(writer.bytes);
}

LayoutStateEvent DecodeLayoutState(std::span<const std::uint8_t> bytes)
{
    Require(bytes.size() >= 11 && bytes.size() <= kMaxLayoutStatePayload);
    Reader reader{bytes};
    Require(reader.U(2) == kLayoutControlVersion);
    LayoutStateEvent result;
    result.subscription = reader.U(8);
    result.status = static_cast<LayoutStateStatus>(reader.U(1));
    Require(result.subscription && static_cast<unsigned>(result.status) <=
                                       static_cast<unsigned>(LayoutStateStatus::Disconnected));
    if (result.status == LayoutStateStatus::Current) {
        result.snapshot = DecodeLayoutSnapshot(bytes.subspan(reader.at));
    } else {
        Require(reader.at == bytes.size());
    }
    return result;
}
} // namespace prism::contracts
