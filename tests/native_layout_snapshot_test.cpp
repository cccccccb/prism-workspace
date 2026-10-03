#include "fixtures/wm_theme_fixture.hpp"
#include "prism/contracts/layout_snapshot.hpp"
#include "prism/ipc/ipc_server.hpp"
#include "prism/launch/control_protocol.hpp"
#include "prism/launch/stream.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/wlr_server.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
}

namespace {
using namespace prism;
using namespace std::chrono_literals;

class Client {
public:
    Client(std::string socket, int number) : socket_(std::move(socket)), number_(number)
    {
        thread_ = std::thread(&Client::Run, this);
    }

    ~Client()
    {
        stop = true;
        thread_.join();
    }

    std::atomic<bool> stop{}, pause{}, paused{}, ready{};

private:
    static void Paint(void *data, int width, int height, int stride)
    {
        auto *pixels = static_cast<std::uint32_t *>(data);
        for (int y = 0; y < height; ++y) {
            std::fill_n(pixels + y * stride / 4, width, 0xFF557788);
        }
    }

    void Run()
    {
        platform::WaylandWindow window;
        window.SetPaintHandler(Paint);
        if (!window.Open(socket_, "prism.layout-" + std::to_string(number_), "Layout fixture", 480,
                         300)) {
            return;
        }
        ready = true;
        while (!stop) {
            if (pause) {
                paused = true;
                std::this_thread::sleep_for(1ms);
                continue;
            }
            paused = false;
            if (!window.Pump(5)) {
                break;
            }
        }
    }

    std::string socket_;
    int number_;
    std::thread thread_;
};

template <class Predicate> void Until(wm::WlrServer &server, Predicate condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        server.RunEventLoopIteration(5);
    }
    assert(condition());
}

nlohmann::json Command(wm::WlrServer &server, const std::string &command)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto &path = server.GetIpcServer()->GetSocketPath();
    assert(path.size() < sizeof(address.sun_path));
    std::copy(path.begin(), path.end(), address.sun_path);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);

    const auto request = command + "\n";
    assert(send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
    std::string response;
    Until(server, [&] {
        char bytes[4096];
        const auto size = recv(fd, bytes, sizeof(bytes), 0);
        if (size > 0) {
            response.append(bytes, size);
        }
        return response.find('\n') != std::string::npos;
    });
    close(fd);
    return nlohmann::json::parse(response);
}

const contracts::LayoutNode *FindNode(const contracts::LayoutSnapshot &snapshot, std::uint64_t id)
{
    const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
                                    [id](const auto &node) { return node.id == id; });
    return found == snapshot.nodes.end() ? nullptr : &*found;
}

std::uint64_t Focused(const contracts::LayoutSnapshot &snapshot)
{
    std::uint64_t result{};
    for (const auto &node : snapshot.nodes) {
        if (node.focused) {
            assert(!result);
            assert(node.kind == contracts::LayoutNodeKind::View && node.visible);
            result = node.id;
        }
    }
    return result;
}

std::size_t ViewCount(const contracts::LayoutSnapshot &snapshot)
{
    return std::count_if(snapshot.nodes.begin(), snapshot.nodes.end(), [](const auto &node) {
        return node.kind == contracts::LayoutNodeKind::View;
    });
}

bool Settled(wm::WlrServer &server, std::size_t count)
{
    const auto snapshot = server.GetLayoutSnapshot();
    if (!snapshot || ViewCount(*snapshot) != count) {
        return false;
    }
    for (const auto &node : snapshot->nodes) {
        if (node.kind == contracts::LayoutNodeKind::View && node.visible &&
            (!node.has_committed || node.target_bounds != node.committed_bounds)) {
            return false;
        }
    }
    return true;
}

class ControlPeer {
public:
    explicit ControlPeer(int fd) : stream(fd, launch::ControlFrameSize)
    {
    }

    void Collect()
    {
        for (const auto &frame : stream.Receive()) {
            auto message = launch::DecodeControl(frame);
            if (message.type == launch::ControlType::LayoutSnapshot) {
                contracts::ValidateLayoutSnapshot(message.layout_snapshot);
                snapshots.push_back(std::move(message.layout_snapshot));
            } else {
                messages.push_back(std::move(message));
            }
        }
    }

    void Send(launch::ControlMessage message)
    {
        message.permit.session = session;
        assert(stream.Queue(launch::EncodeControl(message)));
        stream.Flush();
    }

    void Subscribe(bool enabled)
    {
        launch::ControlMessage message;
        message.type = launch::ControlType::LayoutSubscribe;
        message.layout_subscribe = enabled;
        Send(std::move(message));
    }

    launch::Stream stream;
    std::uint64_t session{};
    std::vector<launch::ControlMessage> messages;
    std::vector<contracts::LayoutSnapshot> snapshots;
};

void FindHeadless(wlr_backend *backend, void *data)
{
    if (wlr_backend_is_headless(backend)) {
        *static_cast<wlr_backend **>(data) = backend;
    } else if (wlr_backend_is_multi(backend)) {
        wlr_multi_for_each_backend(backend, FindHeadless, data);
    }
}

wl_iterator_result CountTouch(wl_resource *resource, void *data)
{
    if (std::strcmp(wl_resource_get_class(resource), "wl_touch") == 0) {
        ++*static_cast<int *>(data);
    }
    return WL_ITERATOR_CONTINUE;
}

int TouchResources(wm::WlrServer &server)
{
    int count{};
    wl_client *client;
    wl_client_for_each(client, wl_display_get_client_list(server.GetDisplay()))
        wl_client_for_each_resource(client, CountTouch, &count);
    return count;
}

void TestInputFocus(wm::WlrServer &server, ControlPeer &peer, std::uint64_t first_id,
                    std::uint64_t second_id)
{
    const auto initial = server.GetLayoutSnapshot();
    const auto bounds = FindNode(*initial, second_id)->target_bounds;
    const auto output = initial->outputs.front().logical_bounds;
    const auto x = (bounds.x + bounds.width / 2.0) / output.width;
    const auto y = (bounds.y + bounds.height / 2.0) / output.height;
    server.HandleCursorMotionAbsolute(200, x, y);
    server.HandleCursorButton(201, 272, WL_POINTER_BUTTON_STATE_PRESSED);
    server.HandleCursorButton(202, 272, WL_POINTER_BUTTON_STATE_RELEASED);
    Until(server, [&] { return Focused(*server.GetLayoutSnapshot()) == second_id; });
    assert(server.GetLayoutSnapshot()->active_instance.value == 0);

    launch::ControlMessage activate;
    activate.type = launch::ControlType::Activate;
    activate.permit.instance = {101};
    activate.permit.pid = getpid();
    peer.Send(activate);
    Until(server, [&] {
        peer.Collect();
        return Focused(*server.GetLayoutSnapshot()) == first_id;
    });
    assert(server.GetLayoutSnapshot()->active_instance.value == 101);

    wlr_touch touch{};
    static const wlr_touch_impl touch_impl{.name = "layout-snapshot-touch"};
    wlr_touch_init(&touch, &touch_impl, "layout-snapshot-touch");
    server.HandleNewInput(&touch.base);
    Until(server, [&] { return TouchResources(server) == 2; });
    wlr_touch_down_event down{.touch = &touch, .time_msec = 203, .touch_id = 9, .x = x, .y = y};
    wl_signal_emit_mutable(&touch.events.down, &down);
    wl_signal_emit_mutable(&touch.events.frame, &touch);
    Until(server, [&] { return Focused(*server.GetLayoutSnapshot()) == second_id; });
    assert(wlr_seat_touch_num_points(server.GetSeat()) == 1);
    wlr_touch_up_event up{.touch = &touch, .time_msec = 204, .touch_id = 9};
    wl_signal_emit_mutable(&touch.events.up, &up);
    wl_signal_emit_mutable(&touch.events.frame, &touch);
    wlr_touch_finish(&touch);

    wlr_keyboard keyboard{};
    static const wlr_keyboard_impl keyboard_impl{.name = "layout-snapshot-keyboard",
                                                 .led_update = nullptr};
    wlr_keyboard_init(&keyboard, &keyboard_impl, "layout-snapshot-keyboard");
    server.HandleNewInput(&keyboard.base);
    assert(keyboard.keymap && keyboard.xkb_state);
    const auto logo = xkb_keymap_mod_get_index(keyboard.keymap, XKB_MOD_NAME_LOGO);
    assert(logo != XKB_MOD_INVALID && logo < 32);
    wlr_keyboard_notify_modifiers(&keyboard, 1U << logo, 0, 0, 0);
    wlr_keyboard_key_event key{.time_msec = 205,
                               .keycode = 105,
                               .update_state = false,
                               .state = WL_KEYBOARD_KEY_STATE_PRESSED};
    wlr_keyboard_notify_key(&keyboard, &key);
    key.state = WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_keyboard_notify_key(&keyboard, &key);
    wlr_keyboard_notify_modifiers(&keyboard, 0, 0, 0, 0);
    Until(server, [&] { return Focused(*server.GetLayoutSnapshot()) == first_id; });
    assert(server.GetLayoutSnapshot()->active_instance.value == 101);
    assert(server.GetLayoutSnapshot()->topology_revision == initial->topology_revision);
    assert(server.GetLayoutSnapshot()->layout_revision == initial->layout_revision);
    assert(server.GetLayoutSnapshot()->focus_revision > initial->focus_revision);
    wlr_keyboard_finish(&keyboard);
}

void TestSnapshotState(wm::WlrServer &server, const std::shared_ptr<wm::Compositor> &compositor,
                       ControlPeer &peer)
{
    const auto socket = server.GetSocketName();
    const auto initial = server.GetLayoutSnapshot();
    assert(initial && initial->session == peer.session && initial->revision);
    assert(initial->outputs.size() == 1 && initial->outputs.front().supported);
    assert(initial->workspaces.size() >= 1 && ViewCount(*initial) == 0);
    const auto output_id = initial->outputs.front().id;
    assert(initial->boundaries.empty());
    contracts::ValidateLayoutSnapshot(*initial);

    peer.Subscribe(true);
    Until(server, [&] {
        peer.Collect();
        return !peer.snapshots.empty();
    });
    assert(peer.snapshots.back() == *server.GetLayoutSnapshot());

    launch::ControlMessage grant;
    grant.type = launch::ControlType::Grant;
    grant.permit.request = {1};
    grant.permit.instance = {101};
    grant.permit.pid = getpid();
    grant.permit.expires_ns = launch::MonotonicNs() + 30'000'000'000ULL;
    peer.Send(grant);
    Until(server, [&] {
        peer.Collect();
        return peer.messages.size() >= 2;
    });
    assert(peer.messages.back().type == launch::ControlType::Registered);
    assert(peer.messages.back().success);

    auto first = std::make_unique<Client>(socket, 1);
    Until(server, [&] { return first->ready && Settled(server, 1); });
    const auto one = server.GetLayoutSnapshot();
    const auto first_id = Focused(*one);
    assert(first_id && one->active_instance.value == 101);
    assert(FindNode(*one, first_id)->instance.value == 101);
    assert(one->topology_revision > initial->topology_revision);
    assert(ViewCount(*initial) == 0); // Retained snapshots remain immutable.

    auto second = std::make_unique<Client>(socket, 2);
    Until(server, [&] { return second->ready && Settled(server, 2); });
    const auto two = server.GetLayoutSnapshot();
    const auto second_id = Focused(*two);
    assert(second_id && second_id != first_id);
    assert(two->active_instance.value == 0);
    assert(two->boundaries.size() == 1 && two->boundaries.front().resizable);
    const auto boundary_id = two->boundaries.front().id;

    assert(Command(server, "focus left")["status"] == "ok");
    Until(server, [&] { return Focused(*server.GetLayoutSnapshot()) == first_id; });
    const auto focused = server.GetLayoutSnapshot();
    assert(focused->active_instance.value == 101);
    assert(focused->topology_revision == two->topology_revision);
    assert(focused->layout_revision == two->layout_revision);
    assert(focused->focus_revision > two->focus_revision);
    assert(focused->boundaries.front().id == boundary_id);

    TestInputFocus(server, peer, first_id, second_id);

    // Freeze a real client before reconfiguration: desired geometry must not be
    // confused with its most recently committed buffer geometry.
    first->pause = true;
    Until(server, [&] { return first->paused.load(); });
    const auto old_bounds = FindNode(*server.GetLayoutSnapshot(), first_id)->committed_bounds;
    auto theme = test::WmThemeFixture(2);
    theme.layout = {36, 64, 500, 8, 6};
    assert(server.InstallTheme(theme).success);
    Until(server, [&] {
        return FindNode(*server.GetLayoutSnapshot(), first_id)->target_bounds != old_bounds;
    });
    const auto pending = server.GetLayoutSnapshot();
    assert(FindNode(*pending, first_id)->committed_bounds.width == old_bounds.width);
    assert(FindNode(*pending, first_id)->committed_bounds.height == old_bounds.height);
    assert(FindNode(*pending, first_id)->target_bounds !=
           FindNode(*pending, first_id)->committed_bounds);
    assert(pending->topology_revision == focused->topology_revision);
    assert(pending->layout_revision > focused->layout_revision);
    assert(pending->boundaries.front().id == boundary_id);
    first->pause = false;
    Until(server, [&] { return Settled(server, 2); });
    const auto committed = server.GetLayoutSnapshot();
    assert(committed->revision > pending->revision);
    assert(committed->layout_revision == pending->layout_revision);
    assert(committed->topology_revision == pending->topology_revision);
    assert(committed->focus_revision == pending->focus_revision);
    assert(FindNode(*pending, first_id)->target_bounds !=
           FindNode(*pending, first_id)->committed_bounds);

    assert(Command(server, "fullscreen on")["fullscreen"] == true);
    Until(server, [&] { return Settled(server, 2); });
    const auto fullscreen = server.GetLayoutSnapshot();
    assert(FindNode(*fullscreen, first_id)->fullscreen);
    assert(FindNode(*fullscreen, first_id)->visible);
    assert(!FindNode(*fullscreen, second_id)->visible);
    assert(fullscreen->workspaces.front().mode == contracts::LayoutGroupMode::Normal);
    assert(fullscreen->workspaces.front().mode_revision == 1);
    assert(Command(server, "fullscreen off")["fullscreen"] == false);
    Until(server, [&] { return Settled(server, 2); });

    assert(Command(server, "move_workspace 2")["status"] == "ok");
    Until(server, [&] { return Settled(server, 2); });
    const auto moved = server.GetLayoutSnapshot();
    assert(FindNode(*moved, first_id) && !FindNode(*moved, first_id)->visible);
    assert(FindNode(*moved, first_id)->workspace != FindNode(*moved, second_id)->workspace);
    assert(Focused(*moved) == second_id);
    assert(moved->boundaries.empty());
    assert(Command(server, "workspace 2")["status"] == "ok");
    Until(server, [&] { return Settled(server, 2); });
    assert(Focused(*server.GetLayoutSnapshot()) == first_id);
    assert(server.GetLayoutSnapshot()->active_instance.value == 101);

    first.reset();
    Until(server, [&] { return ViewCount(*server.GetLayoutSnapshot()) == 1; });
    const auto unmapped = server.GetLayoutSnapshot();
    assert(!FindNode(*unmapped, first_id));
    assert(Focused(*unmapped) == 0 && unmapped->active_instance.value == 0);
    assert(Command(server, "workspace 1")["status"] == "ok");
    Until(server, [&] { return Settled(server, 1); });
    assert(Focused(*server.GetLayoutSnapshot()) == second_id);

    // Destroy/recreate allocates a fresh output identity independently of any
    // backend object address or display-name reuse.
    wlr_backend *headless{};
    FindHeadless(server.GetBackend(), &headless);
    assert(headless);
    auto *old_output = wlr_output_layout_get_center_output(server.GetOutputLayout());
    assert(old_output);
    wlr_output_destroy(old_output);
    Until(server, [&] { return server.GetLayoutSnapshot()->outputs.empty(); });
    auto *new_output = wlr_headless_add_output(headless, 1024, 768);
    assert(new_output);
    Until(server,
          [&] { return server.GetLayoutSnapshot()->outputs.size() == 1 && Settled(server, 1); });
    const auto recreated = server.GetLayoutSnapshot();
    assert(recreated->outputs.front().id != output_id);
    assert(recreated->outputs.front().logical_bounds.width == 1024);
    assert(recreated->outputs.front().logical_bounds.height == 768);
    assert(FindNode(*recreated, second_id));

    // Native output scaling changes logical target geometry, even though the
    // underlying headless output remains 1024x768 physical pixels.
    wlr_output_state output_state{};
    wlr_output_state_init(&output_state);
    wlr_output_state_set_scale(&output_state, 2.0f);
    assert(wlr_output_commit_state(new_output, &output_state));
    wlr_output_state_finish(&output_state);
    Until(server, [&] {
        return server.GetLayoutSnapshot()->outputs.front().logical_bounds.width == 512 &&
               Settled(server, 1);
    });
    const auto scaled = server.GetLayoutSnapshot();
    assert(scaled->outputs.front().id == recreated->outputs.front().id);
    assert(scaled->outputs.front().scale == 2.0);
    assert(scaled->outputs.front().logical_bounds.height == 384);
    assert(scaled->topology_revision == recreated->topology_revision);
    assert(scaled->layout_revision > recreated->layout_revision);
    const auto &scaled_node = *FindNode(*scaled, second_id);
    assert(scaled_node.target_bounds.x + scaled_node.target_bounds.width <= 512);
    assert(scaled_node.target_bounds.y + scaled_node.target_bounds.height <= 384);

    wlr_output_state_init(&output_state);
    wlr_output_state_set_scale(&output_state, 1.0f);
    assert(wlr_output_commit_state(new_output, &output_state));
    wlr_output_state_finish(&output_state);
    Until(server, [&] {
        return server.GetLayoutSnapshot()->outputs.front().logical_bounds.width == 1024 &&
               Settled(server, 1);
    });
    const auto idle = server.GetLayoutSnapshot();
    for (int turn = 0; turn < 4; ++turn) {
        server.RunEventLoopIteration(5);
        assert(server.GetLayoutSnapshot() == idle);
    }

    // Output relocation moves the existing committed surface. A paused client
    // cannot provide a replacement buffer, so equality proves this path needs
    // no client resize acknowledgement or extra pixel submission.
    second->pause = true;
    Until(server, [&] { return second->paused.load(); });
    const auto before_move = server.GetLayoutSnapshot();
    const auto old_target = FindNode(*before_move, second_id)->target_bounds;
    assert(wlr_output_layout_add(server.GetOutputLayout(), new_output, -100, 50));
    const auto relocated = server.GetLayoutSnapshot();
    const auto &relocated_node = *FindNode(*relocated, second_id);
    assert(relocated->outputs.front().id == before_move->outputs.front().id);
    assert(relocated->outputs.front().logical_bounds.x == -100);
    assert(relocated->outputs.front().logical_bounds.y == 50);
    assert(relocated->topology_revision == before_move->topology_revision);
    assert(relocated->layout_revision > before_move->layout_revision);
    assert(relocated_node.target_bounds.x == old_target.x - 100);
    assert(relocated_node.target_bounds.y == old_target.y + 50);
    assert(relocated_node.target_bounds.width == old_target.width);
    assert(relocated_node.target_bounds.height == old_target.height);
    assert(relocated_node.committed_bounds == relocated_node.target_bounds);
    assert(FindNode(*before_move, second_id)->target_bounds == old_target);

    assert(wlr_output_layout_add(server.GetOutputLayout(), new_output, 0, 0));
    const auto restored = server.GetLayoutSnapshot();
    assert(FindNode(*restored, second_id)->target_bounds == old_target);
    assert(FindNode(*restored, second_id)->committed_bounds == old_target);
    second->pause = false;

    // First drain the ordered publication stream, then prove that unsubscribe
    // suppresses updates and resubscribe sends one complete current snapshot.
    Until(server, [&] {
        peer.Collect();
        return !peer.snapshots.empty() &&
               peer.snapshots.back().revision == server.GetLayoutSnapshot()->revision;
    });
    for (std::size_t index = 1; index < peer.snapshots.size(); ++index) {
        assert(peer.snapshots[index].revision > peer.snapshots[index - 1].revision);
    }
    peer.Subscribe(false);
    server.RunEventLoopIteration(0);
    peer.Collect();
    const auto published_count = peer.snapshots.size();
    assert(Command(server, "workspace 3")["status"] == "ok");
    for (int attempt = 0; attempt < 3; ++attempt) {
        server.RunEventLoopIteration(0);
        peer.Collect();
    }
    assert(peer.snapshots.size() == published_count);
    peer.Subscribe(true);
    Until(server, [&] {
        peer.Collect();
        return peer.snapshots.size() > published_count;
    });
    assert(peer.snapshots.back() == *server.GetLayoutSnapshot());

    second.reset();
    Until(server, [&] { return compositor->GetWindows().empty(); });
    const auto final = server.GetLayoutSnapshot();
    assert(ViewCount(*final) == 0 && final->boundaries.empty());
    assert(!FindNode(*final, first_id) && !FindNode(*final, second_id));
    contracts::ValidateLayoutSnapshot(*final);

    launch::ControlMessage stale;
    stale.type = launch::ControlType::LayoutSubscribe;
    stale.layout_subscribe = true;
    stale.permit.session = peer.session ^ 1ULL;
    if (!stale.permit.session) {
        stale.permit.session = 2;
    }
    assert(peer.stream.Queue(launch::EncodeControl(stale)));
    peer.stream.Flush();
    Until(server, [&] { return !server.ControlHealthy(); });
    Until(server, [&] {
        peer.Collect();
        return peer.stream.Closed();
    });
}

void RunChild(int server_fd, int peer_fd, const char *directory)
{
    setenv("XDG_RUNTIME_DIR", directory, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("XKB_DEFAULT_LAYOUT", "us", 1);
    auto compositor = std::make_shared<wm::Compositor>();
    assert(compositor->Initialize());
    wm::WlrServer server(compositor);
    assert(server.Initialize("wayland-layout-" + std::to_string(getpid())));
    server.AttachControl(server_fd, getppid());
    server.Start();
    assert(server.InstallTheme(test::WmThemeFixture()).success);

    ControlPeer peer(peer_fd);
    peer.Collect();
    assert(peer.messages.size() == 1 && peer.messages.front().type == launch::ControlType::Ready);
    peer.session = peer.messages.front().permit.session;
    TestSnapshotState(server, compositor, peer);
    server.Stop();
}
} // namespace

int main()
{
    char directory[] = "/tmp/prism-native-layout.XXXXXX";
    assert(mkdtemp(directory));
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const auto child = fork();
    assert(child >= 0);
    if (!child) {
        // Both endpoints originate from the actual parent before fork. The WM
        // endpoint follows production SO_PEERCRED/getppid verification.
        RunChild(sockets[1], sockets[0], directory);
        _exit(0);
    }
    close(sockets[0]);
    close(sockets[1]);
    int status{};
    assert(waitpid(child, &status, 0) == child);
    std::filesystem::remove_all(directory);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::puts("Native layout snapshots: XDG focus/map/unmap, immutable target/commit geometry, "
              "workspace/fullscreen/theme, output recreation and inherited subscription passed");
}
