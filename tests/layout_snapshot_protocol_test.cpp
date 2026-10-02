#include "prism/contracts/layout_snapshot.hpp"
#include "prism/launch/control_protocol.hpp"
#include "prism/launch/stream.hpp"
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>

using namespace prism;
using namespace contracts;

namespace {
void Reject(const std::function<void()> &operation)
{
    bool rejected{};
    try {
        operation();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

LayoutSnapshot Fixture()
{
    LayoutSnapshot snapshot;
    snapshot.session = 0x123456789ULL;
    snapshot.revision = 9;
    snapshot.topology_revision = 4;
    snapshot.layout_revision = 7;
    snapshot.focus_revision = 3;
    snapshot.active_instance = {201};
    snapshot.outputs.push_back({11, "HEADLESS-1", {0, 0, 1920, 1080}, 1.25, true, true});
    snapshot.workspaces.push_back({31, 41, 11, "Main", true});

    LayoutNode root;
    root.id = 41;
    root.workspace = 31;
    root.layout = LayoutArrangement::Horizontal;
    root.children = {42, 43, 44};
    root.tile_bounds = {12, 50, 1896, 960};
    root.target_bounds = root.tile_bounds;
    root.visible = true;
    snapshot.nodes.push_back(root);
    for (unsigned i = 0; i < 3; ++i) {
        LayoutNode node;
        node.id = 42 + i;
        node.parent = 41;
        node.workspace = 31;
        node.kind = LayoutNodeKind::View;
        node.tile_bounds = {12.0 + 632.0 * i, 50, 632, 960};
        node.target_bounds = node.tile_bounds;
        node.committed_bounds = {node.tile_bounds.x, 50, 600, 940};
        node.width_fraction = 1.0 / 3;
        node.height_fraction = 1;
        node.instance = {201 + i};
        node.focused = i == 0;
        node.visible = true;
        node.has_committed = true;
        snapshot.nodes.push_back(node);
    }
    snapshot.boundaries.push_back(
        {71, 41, 42, 43, 31, LayoutBoundaryAxis::X, {640, 50, 8, 960}, true, false});
    snapshot.boundaries.push_back(
        {72, 41, 43, 44, 31, LayoutBoundaryAxis::X, {1272, 50, 8, 960}, true, false});
    return snapshot;
}

void RoundTrip()
{
    const auto snapshot = Fixture();
    const auto bytes = EncodeLayoutSnapshot(snapshot);
    assert(DecodeLayoutSnapshot(bytes) == snapshot);
    assert(bytes.size() < kMaxLayoutSnapshotPayload);
    assert(snapshot.nodes[1].target_bounds != snapshot.nodes[1].committed_bounds);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        Reject([&] { DecodeLayoutSnapshot(std::span(bytes).first(size)); });
    }

    auto malformed = bytes;
    malformed.push_back(0);
    Reject([&] { DecodeLayoutSnapshot(malformed); });
    malformed = bytes;
    malformed[1] = kLayoutSnapshotVersion + 1;
    Reject([&] { DecodeLayoutSnapshot(malformed); });
    malformed = bytes;
    malformed[54] = 0xff;
    malformed[55] = 0xff;
    Reject([&] { DecodeLayoutSnapshot(malformed); });
    malformed = bytes;
    malformed[50] = 0xff;
    malformed[51] = 0xff;
    Reject([&] { DecodeLayoutSnapshot(malformed); });
    malformed = bytes;
    malformed.back() = 2; // Boolean resizable must not accept arbitrary nonzero bytes.
    Reject([&] { DecodeLayoutSnapshot(malformed); });
    malformed.assign(kMaxLayoutSnapshotPayload + 1, 0);
    Reject([&] { DecodeLayoutSnapshot(malformed); });

    LayoutSnapshot empty;
    empty.session = empty.revision = empty.topology_revision = empty.layout_revision =
        empty.focus_revision = 1;
    assert(DecodeLayoutSnapshot(EncodeLayoutSnapshot(empty)) == empty);
}

void InvalidIdentitiesAndGraph()
{
    auto invalid = Fixture();
    invalid.nodes[2].id = invalid.nodes[1].id;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[1].parent = 999;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[1].workspace = 999;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.workspaces[0].root = invalid.nodes[1].id;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[0].children[1] = invalid.nodes[0].children[0];
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[0].children.pop_back(); // An orphan cannot be hidden in the flat node array.
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[0].children[0] = invalid.nodes[0].id;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.workspaces[0].output = 999;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.outputs.push_back(invalid.outputs.front());
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.workspaces.push_back(invalid.workspaces.front());
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[2].focused = true;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.active_instance = {999};
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.active_instance = {}; // A registered focused view must identify its active instance.
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[1].focused = false;
    invalid.nodes[0].focused = true;
    invalid.active_instance = {};
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[1].visible = false;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[3].visible = false; // Its adjacent visible handle must disappear too.
    Reject([&] { EncodeLayoutSnapshot(invalid); });

    auto external = Fixture();
    external.nodes[1].instance = {};
    external.active_instance = {};
    assert(DecodeLayoutSnapshot(EncodeLayoutSnapshot(external)) == external);
}

void InvalidGeometryAndBoundaries()
{
    auto invalid = Fixture();
    invalid.nodes[1].target_bounds.x = std::numeric_limits<double>::quiet_NaN();
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.outputs[0].scale = std::numeric_limits<double>::infinity();
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[1].committed_bounds.width = -1;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[1].has_committed = false;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.boundaries[1].id = invalid.boundaries[0].id;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.boundaries[0].second = 44; // Nonadjacent children cannot form a boundary.
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.boundaries.pop_back();
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.boundaries[0].axis = LayoutBoundaryAxis::Y;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.boundaries[0].resizable = true;
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.nodes[1].kind = static_cast<LayoutNodeKind>(200);
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.workspaces[0].mode = static_cast<LayoutGroupMode>(2);
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.outputs[0].name = std::string("a\0b", 3);
    Reject([&] { EncodeLayoutSnapshot(invalid); });
    invalid = Fixture();
    invalid.outputs[0].name = "\xc0\x80";
    Reject([&] { EncodeLayoutSnapshot(invalid); });
}

void ControlTransport()
{
    launch::ControlMessage subscribe;
    subscribe.type = launch::ControlType::LayoutSubscribe;
    subscribe.permit.session = Fixture().session;
    subscribe.layout_subscribe = true;
    auto encoded = launch::EncodeControl(subscribe);
    assert(launch::ControlFrameSize(encoded) == 21);
    auto decoded = launch::DecodeControl(encoded);
    assert(decoded.type == subscribe.type && decoded.layout_subscribe);
    subscribe.layout_subscribe = false;
    assert(!launch::DecodeControl(launch::EncodeControl(subscribe)).layout_subscribe);
    encoded.back() = 2;
    Reject([&] { launch::DecodeControl(encoded); });

    launch::ControlMessage message;
    message.type = launch::ControlType::LayoutSnapshot;
    message.permit.session = Fixture().session;
    message.layout_snapshot = Fixture();
    encoded = launch::EncodeControl(message);
    decoded = launch::DecodeControl(encoded);
    assert(decoded.type == message.type && decoded.layout_snapshot == message.layout_snapshot);
    auto invalid = message;
    invalid.permit.session++;
    Reject([&] { launch::EncodeControl(invalid); });
    auto bad_frame = encoded;
    bad_frame[19] ^= 1; // Outer connection session cannot disagree with inner snapshot.
    Reject([&] { launch::DecodeControl(bad_frame); });
    bad_frame = encoded;
    bad_frame[8] = 1; // Reject excessive advertised body before buffering the full payload.
    Reject([&] { launch::ControlFrameSize(bad_frame); });

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    launch::Stream sender(sockets[0], launch::ControlFrameSize);
    launch::Stream receiver(sockets[1], launch::ControlFrameSize);
    assert(sender.Queue(encoded));
    sender.Flush();
    const auto frames = receiver.Receive();
    assert(frames.size() == 1);
    assert(launch::DecodeControl(frames[0]).layout_snapshot == message.layout_snapshot);
    std::vector<std::uint8_t> oversized(256 * 1024 + 1);
    assert(!sender.Queue(oversized) && sender.Closed());

    for (unsigned type = 1; type <= 8; ++type) {
        launch::ControlMessage old;
        old.type = static_cast<launch::ControlType>(type);
        old.permit.session = 1;
        old.success = true;
        const auto bytes = launch::EncodeControl(old);
        assert(bytes.size() == 82);
        assert(launch::DecodeControl(bytes).type == old.type);
    }
}

void DisplayNames()
{
    assert(LayoutDisplayName("") == "Unnamed");
    assert(LayoutDisplayName("Music 工作区") == "Music 工作区");
    assert(LayoutDisplayName(std::string(200, 'a')) == std::string(128, 'a'));
    assert(LayoutDisplayName(std::string(127, 'a') + "界") == std::string(127, 'a'));
    assert(LayoutDisplayName(std::string(125, 'a') + "界") == std::string(125, 'a') + "界");
    assert(LayoutDisplayName("before\nnext") == "before\xef\xbf\xbdnext");
    assert(LayoutDisplayName(std::string("a\0b", 3)) == "a\xef\xbf\xbd"
                                                        "b");
    assert(LayoutDisplayName("a\xc2\x85z") == "a\xef\xbf\xbdz");
    assert(LayoutDisplayName("a\xffz") == "a\xef\xbf\xbdz");
    assert(LayoutDisplayName("a\xe7\x95") == "a\xef\xbf\xbd\xef\xbf\xbd");

    auto snapshot = Fixture();
    snapshot.outputs[0].name = LayoutDisplayName(std::string(126, 'o') + "\xff");
    snapshot.workspaces[0].name = LayoutDisplayName("\xed\xa0\x80");
    assert(DecodeLayoutSnapshot(EncodeLayoutSnapshot(snapshot)) == snapshot);
}

void CapacityBounds()
{
    auto snapshot = Fixture();
    snapshot.nodes.resize(1);
    snapshot.nodes[0].children.clear();
    snapshot.boundaries.clear();
    snapshot.active_instance = {};
    for (std::size_t i = 1; i < kMaxLayoutNodes; ++i) {
        LayoutNode node;
        node.id = 1000 + i;
        node.parent = snapshot.nodes[0].id;
        node.workspace = snapshot.workspaces[0].id;
        node.kind = LayoutNodeKind::View;
        snapshot.nodes[0].children.push_back(node.id);
        snapshot.nodes.push_back(node);
        if (i > 1) {
            LayoutBoundary boundary;
            boundary.id = 2000 + i;
            boundary.parent = snapshot.nodes[0].id;
            boundary.first = node.id - 1;
            boundary.second = node.id;
            boundary.workspace = node.workspace;
            snapshot.boundaries.push_back(boundary);
        }
    }
    const auto bytes = EncodeLayoutSnapshot(snapshot);
    assert(bytes.size() < kMaxLayoutSnapshotPayload);
    assert(DecodeLayoutSnapshot(bytes) == snapshot);
    snapshot.nodes.push_back({});
    Reject([&] { EncodeLayoutSnapshot(snapshot); });

    snapshot = Fixture();
    snapshot.nodes[0].children.assign(kMaxLayoutNodes + 1, 42);
    Reject([&] { EncodeLayoutSnapshot(snapshot); });
    snapshot = Fixture();
    snapshot.outputs[0].name.assign(129, 'x');
    Reject([&] { EncodeLayoutSnapshot(snapshot); });
}

void OrderedImmutableCache()
{
    launch::LayoutSnapshotCache cache;
    auto first = Fixture();
    Reject([&] { cache.Accept(first); });
    cache.Reset(first.session);
    cache.Accept(first);
    const auto retained = cache.Current();
    first.nodes[1].target_bounds.width = 500;
    assert(retained->nodes[1].target_bounds.width == 632);
    Reject([&] { cache.Accept(first); });
    first.revision++;
    first.layout_revision++;
    cache.Accept(first);
    assert(cache.Current()->nodes[1].target_bounds.width == 500);
    assert(retained->nodes[1].target_bounds.width == 632);

    auto stale = first;
    stale.revision++;
    stale.topology_revision--;
    Reject([&] { cache.Accept(stale); });
    stale = first;
    stale.revision++;
    stale.focus_revision--;
    Reject([&] { cache.Accept(stale); });
    stale = first;
    stale.revision++;
    stale.session++;
    Reject([&] { cache.Accept(stale); });
    assert(*cache.Current() == first);

    cache.Reset();
    assert(!cache.Current());
    Reject([&] { cache.Accept(first); });
    cache.Reset(first.session + 1);
    first.session++;
    first.revision = 1;
    cache.Accept(first);
    assert(cache.Current()->revision == 1);
    assert(retained->session + 1 == cache.Current()->session);
}
} // namespace

int main()
{
    RoundTrip();
    InvalidIdentitiesAndGraph();
    InvalidGeometryAndBoundaries();
    ControlTransport();
    DisplayNames();
    CapacityBounds();
    OrderedImmutableCache();
}
