#pragma once
#include "prism/contracts/layout_control.hpp"
#include "prism/launch/control_protocol.hpp"
#include "prism/launch/stream.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/wlr_server.hpp"
#include "wm_theme_fixture.hpp"
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
}

namespace {
using namespace prism;
using namespace std::chrono_literals;
using contracts::LayoutControlError;
using contracts::LayoutControlPhase;
using contracts::LayoutControlStatus;

// The fixture transport reports actual protocol credentials received by an
// exec-isolated client. It is not an alternative production input protocol.
enum class PacketKind : std::uint32_t { Ready, Input, Barrier };

struct Packet {
    PacketKind kind{};
    contracts::LayoutInputProof proof;
    std::uint32_t buttons{}, keys{}, configures{};
};

class ClientEvents {
public:
    explicit ClientEvents(int fd) : fd_(fd)
    {
    }

    void Event(const contracts::WindowEvent &event)
    {
        if (const auto *button = std::get_if<contracts::PointerButtonEvent>(&event);
            button && button->button == contracts::PointerButton::Primary &&
            button->state == contracts::ButtonState::Pressed) {
            Send({PacketKind::Input,
                  {contracts::LayoutInputKind::Pointer, button->protocol_serial, 0}});
        }
    }

    void Send(Packet packet)
    {
        assert(send(fd_, &packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet));
    }

    static void Paint(void *data, int width, int height, int stride)
    {
        auto *pixels = static_cast<std::uint32_t *>(data);
        for (int y = 0; y < height; ++y) {
            std::fill_n(pixels + y * stride / 4, width, 0xFF557788);
        }
    }

private:
    int fd_;
};

int RunClient(const char *socket, int channel, bool material = false)
{
    char start{};
    assert(recv(channel, &start, 1, 0) == 1 && start == 'S');
    ClientEvents events(channel);
    platform::WaylandWindow window;
    window.SetPaintHandler(ClientEvents::Paint);
    window.SetEventHandler(std::bind_front(&ClientEvents::Event, &events));
    assert(window.Open(socket, "prism.control-fixture", "Control fixture", 480, 300));
    if (material) {
        const contracts::SurfaceEffectRegion region{{0, 0, 168, 56}, 8, 4};
        window.SetSurfaceEffects(std::span(&region, 1));
    }
    events.Send({PacketKind::Ready, {}});

    pollfd wake{channel, POLLIN, 0};
    while (window.Pump(5, std::span(&wake, 1))) {
        if (wake.revents & POLLIN) {
            char command{};
            assert(recv(channel, &command, 1, 0) == 1);
            if (command == 'Q') {
                break;
            }
            assert(command == 'B');
            assert(wl_display_roundtrip(window.Display()) >= 0);
            events.Send({PacketKind::Barrier,
                         {},
                         static_cast<std::uint32_t>(window.PointerButtonCount()),
                         static_cast<std::uint32_t>(window.KeyCount()),
                         static_cast<std::uint32_t>(window.ConfigureCount())});
        }
        if (wake.revents & (POLLHUP | POLLERR)) {
            break;
        }
    }
    window.Close();
    close(channel);
    return 0;
}

class ClientProcess {
public:
    explicit ClientProcess(const std::string &socket, bool minimum = false)
    {
        int channels[2];
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channels) == 0);
        const auto parent = getpid();
        pid = fork();
        assert(pid >= 0);
        if (!pid) {
            assert(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0);
            if (getppid() != parent) {
                _exit(125);
            }
            close(channels[0]);
            const auto descriptor = std::to_string(channels[1]);
            execl("/proc/self/exe", "native_layout_fixture",
                  minimum ? "--minimum-client" : "--client", socket.c_str(), descriptor.c_str(),
                  nullptr);
            _exit(127);
        }
        close(channels[1]);
        channel_ = channels[0];
    }

    ~ClientProcess()
    {
        close(channel_);
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }

    void Start()
    {
        Command('S');
    }

    void Stop()
    {
        Command('Q');
    }

    bool Reaped()
    {
        if (pid < 0) {
            return true;
        }
        int status{};
        const auto result = waitpid(pid, &status, WNOHANG);
        assert(result >= 0);
        if (!result) {
            return false;
        }
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        pid = -1;
        return true;
    }

    void Collect()
    {
        for (;;) {
            Packet packet;
            const auto bytes = recv(channel_, &packet, sizeof(packet), MSG_DONTWAIT);
            if (bytes == 0 || (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                return;
            }
            assert(bytes == sizeof(packet));
            if (packet.kind == PacketKind::Ready) {
                ready = true;
            } else if (packet.kind == PacketKind::Barrier) {
                ++barriers;
                last = packet;
            } else {
                assert(packet.kind == PacketKind::Input && packet.proof.serial);
                inputs.push_back(packet.proof);
            }
        }
    }

    void Minimum()
    {
        Command('M');
    }

    void Barrier()
    {
        Command('B');
    }

    Packet last;
    std::uint32_t barriers{};
    pid_t pid{-1};
    bool ready{};
    std::vector<contracts::LayoutInputProof> inputs;

private:
    void Command(char command)
    {
        assert(send(channel_, &command, 1, MSG_NOSIGNAL) == 1);
    }

    int channel_{-1};
};

template <class Predicate> void Until(wm::WlrServer &server, Predicate condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        server.RunEventLoopIteration(5);
    }
    assert(condition());
}

class ControlPeer {
public:
    explicit ControlPeer(int fd) : stream(fd, launch::ControlFrameSize)
    {
    }

    void Collect()
    {
        for (const auto &frame : stream.Receive()) {
            messages.push_back(launch::DecodeControl(frame));
        }
        assert(!stream.Closed());
    }

    void Send(launch::ControlMessage message)
    {
        message.permit.session = session;
        assert(stream.Queue(launch::EncodeControl(message)));
        stream.Flush();
    }

    bool Has(launch::ControlType type, contracts::InstanceId instance)
    {
        Collect();
        return std::any_of(messages.begin(), messages.end(), [type, instance](const auto &message) {
            return message.type == type && message.permit.instance == instance;
        });
    }

    contracts::LayoutControlResult Apply(wm::WlrServer &server, const launch::ShellPermit &permit,
                                         const contracts::LayoutControlRequest &request)
    {
        launch::ControlMessage message;
        message.type = launch::ControlType::LayoutControl;
        message.permit = permit;
        message.control_request = request;
        const auto previous = messages.size();
        Send(message);
        Until(server, [&] {
            Collect();
            return std::any_of(
                messages.begin() + previous, messages.end(), [&request](const auto &entry) {
                    return entry.type == launch::ControlType::LayoutControlResult &&
                           entry.control_result.request == request.request &&
                           (entry.control_result.status != LayoutControlStatus::Cancelled ||
                            request.phase == LayoutControlPhase::Cancel);
                });
        });
        for (auto index = previous; index < messages.size(); ++index) {
            const auto &reply = messages[index];
            if (reply.type != launch::ControlType::LayoutControlResult ||
                reply.control_result.request != request.request ||
                (reply.control_result.status == LayoutControlStatus::Cancelled &&
                 request.phase != LayoutControlPhase::Cancel)) {
                continue;
            }
            assert(reply.permit.instance == permit.instance && reply.permit.pid == permit.pid);
            assert(reply.control_result.gesture == request.gesture);
            return reply.control_result;
        }
        std::abort();
    }

    launch::Stream stream;
    std::uint64_t session{};
    std::vector<launch::ControlMessage> messages;
};

class Requests {
public:
    contracts::LayoutControlRequest Begin(wm::WlrServer &server, contracts::LayoutInputProof input)
    {
        const auto snapshot = server.GetLayoutSnapshot();
        const auto workspace =
            std::find_if(snapshot->workspaces.begin(), snapshot->workspaces.end(),
                         [](const auto &entry) { return entry.active; });
        assert(workspace != snapshot->workspaces.end());
        contracts::LayoutControlRequest request;
        request.request = next_++;
        request.gesture = request.request;
        request.sequence = 1;
        request.target = {snapshot->session,
                          workspace->output,
                          workspace->id,
                          workspace->root,
                          0,
                          snapshot->topology_revision,
                          snapshot->layout_revision};
        request.input = input;
        request.position = {320, 20};
        return request;
    }

    contracts::LayoutControlRequest Next(contracts::LayoutControlRequest request,
                                         LayoutControlPhase phase, std::uint64_t session)
    {
        request.request = next_++;
        request.session = session;
        ++request.sequence;
        request.phase = phase;
        return request;
    }

private:
    std::uint64_t next_{1};
};

contracts::LayoutInputProof Press(wm::WlrServer &server, ClientProcess &client,
                                  wlr_input_device *device)
{
    client.Collect();
    const auto count = client.inputs.size();
    const auto output = server.GetLayoutSnapshot()->outputs.front().logical_bounds;
    server.HandleCursorMotionAbsolute(100, 0.5, 20.0 / output.height, device);
    server.HandleCursorButton(101, 272, WL_POINTER_BUTTON_STATE_PRESSED, device);
    Until(server, [&] {
        client.Collect();
        return client.inputs.size() > count;
    });
    assert(client.inputs.back().kind == contracts::LayoutInputKind::Pointer);
    return client.inputs.back();
}

void Release(wm::WlrServer &server, wlr_input_device *device)
{
    server.HandleCursorButton(102, 272, WL_POINTER_BUTTON_STATE_RELEASED, device);
}

// Group-mode scenarios follow below. All input and controls use actual native
// seat events and the inherited private channel; no debug IPC applies modes.

const contracts::LayoutWorkspace &Active(const contracts::LayoutSnapshot &snapshot)
{
    const auto found = std::find_if(snapshot.workspaces.begin(), snapshot.workspaces.end(),
                                    [](const auto &workspace) { return workspace.active; });
    assert(found != snapshot.workspaces.end());
    return *found;
}

const contracts::LayoutNode &View(const contracts::LayoutSnapshot &snapshot,
                                  contracts::InstanceId instance)
{
    const auto found =
        std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(), [instance](const auto &node) {
            return node.kind == contracts::LayoutNodeKind::View && node.instance == instance;
        });
    assert(found != snapshot.nodes.end());
    return *found;
}

void SameTree(const contracts::LayoutSnapshot &before, const contracts::LayoutSnapshot &after)
{
    assert(before.topology_revision == after.topology_revision);
    assert(before.nodes.size() == after.nodes.size());
    for (std::size_t index = 0; index < before.nodes.size(); ++index) {
        const auto &a = before.nodes[index];
        const auto &b = after.nodes[index];
        assert(a.id == b.id && a.parent == b.parent && a.workspace == b.workspace);
        assert(a.kind == b.kind && a.layout == b.layout && a.children == b.children);
        assert(a.width_fraction == b.width_fraction && a.height_fraction == b.height_fraction);
    }
    assert(before.boundaries.size() == after.boundaries.size());
    for (std::size_t index = 0; index < before.boundaries.size(); ++index) {
        const auto &a = before.boundaries[index];
        const auto &b = after.boundaries[index];
        assert(a.id == b.id && a.parent == b.parent && a.first == b.first && a.second == b.second);
    }
}

class Fixture {
public:
    Fixture(wm::WlrServer &server, ControlPeer &peer) : server(server), peer(peer)
    {
        static const wlr_pointer_impl pointer_impl{.name = "group-mode-pointer"};
        wlr_pointer_init(&pointer, &pointer_impl, "group-mode-pointer");
        server.HandleNewInput(&pointer.base);
        static const wlr_keyboard_impl keyboard_impl{.name = "group-mode-keyboard",
                                                     .led_update = nullptr};
        wlr_keyboard_init(&keyboard, &keyboard_impl, "group-mode-keyboard");
        server.HandleNewInput(&keyboard.base);
        assert(keyboard.keymap && keyboard.xkb_state);

        StartTopbar();
        dock = Start(contracts::WindowRole::Dock, dock_permit);
        first = Start(contracts::WindowRole::Toplevel, first_permit);
        second = Start(contracts::WindowRole::Toplevel, second_permit);
        Settle();
    }

    ~Fixture()
    {
        wlr_keyboard_finish(&keyboard);
        wlr_pointer_finish(&pointer);
    }

    std::unique_ptr<ClientProcess> Start(contracts::WindowRole role, launch::ShellPermit &permit,
                                         bool minimum = false)
    {
        auto client = std::make_unique<ClientProcess>(server.GetSocketName(), minimum);
        permit = {};
        permit.session = peer.session;
        permit.request = {next_identity_++};
        permit.instance = {next_identity_++};
        permit.pid = client->pid;
        permit.role = role;
        permit.token.fill(0x72);
        permit.expires_ns = launch::MonotonicNs() + 30'000'000'000ULL;
        launch::ControlMessage grant;
        grant.type = launch::ControlType::Grant;
        grant.permit = permit;
        peer.Send(grant);
        Until(server, [&] { return peer.Has(launch::ControlType::Registered, permit.instance); });
        assert(peer.messages.back().success);
        client->Start();
        Until(server, [&] {
            client->Collect();
            return client->ready && peer.Has(launch::ControlType::Mapped, permit.instance);
        });
        return client;
    }

    void StartTopbar()
    {
        topbar = Start(contracts::WindowRole::TopBar, topbar_permit);
    }

    void Settle()
    {
        Until(server, [&] {
            const auto snapshot = server.GetLayoutSnapshot();
            for (const auto &node : snapshot->nodes) {
                if (node.kind == contracts::LayoutNodeKind::View && node.visible &&
                    (!node.has_committed || node.target_bounds != node.committed_bounds)) {
                    return false;
                }
            }
            return true;
        });
    }

    Packet Sync(ClientProcess &client)
    {
        const auto previous = client.barriers;
        client.Barrier();
        Until(server, [&] {
            client.Collect();
            return client.barriers > previous;
        });
        return client.last;
    }

    void Move(double x, double y)
    {
        const auto output = server.GetLayoutSnapshot()->outputs.front().logical_bounds;
        server.HandleCursorMotionAbsolute(time_++, x / output.width, y / output.height,
                                          &pointer.base);
    }

    void Button(bool pressed)
    {
        server.HandleCursorButton(time_++, 272,
                                  pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                          : WL_POINTER_BUTTON_STATE_RELEASED,
                                  &pointer.base);
    }

    void Key(std::uint32_t code, bool shift = false)
    {
        const auto logo = xkb_keymap_mod_get_index(keyboard.keymap, XKB_MOD_NAME_LOGO);
        const auto shift_index = xkb_keymap_mod_get_index(keyboard.keymap, XKB_MOD_NAME_SHIFT);
        assert(logo < 32 && shift_index < 32);
        const auto modifiers = (1U << logo) | (shift ? (1U << shift_index) : 0);
        wlr_keyboard_notify_modifiers(&keyboard, modifiers, 0, 0, 0);
        wlr_keyboard_key_event key{.time_msec = time_++,
                                   .keycode = code,
                                   .update_state = false,
                                   .state = WL_KEYBOARD_KEY_STATE_PRESSED};
        wlr_keyboard_notify_key(&keyboard, &key);
        key.state = WL_KEYBOARD_KEY_STATE_RELEASED;
        wlr_keyboard_notify_key(&keyboard, &key);
        wlr_keyboard_notify_modifiers(&keyboard, 0, 0, 0, 0);
        Settle();
    }

    void Reveal()
    {
        const auto width = server.GetLayoutSnapshot()->outputs.front().logical_bounds.width;
        Move(width / 2.0, 1);
        Button(true);
        Button(false);
        Sync(*topbar);
    }

    contracts::LayoutControlResult Transition(contracts::LayoutControlIntent intent)
    {
        const auto proof = Press(server, *topbar, &pointer.base);
        Release(server, &pointer.base);
        return CompleteGesture(proof, intent);
    }

    contracts::LayoutControlResult CompleteGesture(contracts::LayoutInputProof proof,
                                                   contracts::LayoutControlIntent intent)
    {
        const auto begin = requests.Begin(server, proof);
        const auto began = peer.Apply(server, topbar_permit, begin);
        assert(began.status == LayoutControlStatus::Began && began.session && !began.applied);
        auto end = requests.Next(begin, LayoutControlPhase::End, began.session);
        end.intent = intent;
        const auto ended = peer.Apply(server, topbar_permit, end);
        assert(ended.status == LayoutControlStatus::Ended &&
               ended.error == LayoutControlError::None);
        assert(ended.applied == (intent != contracts::LayoutControlIntent::None));
        const auto after = server.GetLayoutSnapshot();
        assert(ended.layout_revision == after->layout_revision);
        assert(ended.topology_revision == after->topology_revision);
        assert(peer.Apply(server, topbar_permit, end) == ended);
        last_end_ = end;
        last_result_ = ended;
        Settle();
        return ended;
    }

    void ReplayEnd()
    {
        assert(last_end_.request);
        const auto before = server.GetLayoutSnapshot();
        assert(peer.Apply(server, topbar_permit, last_end_) == last_result_);
        const auto after = server.GetLayoutSnapshot();
        assert(Active(*before).mode == Active(*after).mode);
        assert(Active(*before).mode_revision == Active(*after).mode_revision);
        assert(before->layout_revision == after->layout_revision);
    }

    void Mode(contracts::LayoutGroupMode mode)
    {
        assert(Active(*server.GetLayoutSnapshot()).mode == mode);
    }

    void FocusFirst()
    {
        const auto bounds = View(*server.GetLayoutSnapshot(), first_permit.instance).target_bounds;
        Move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
        Button(true);
        Button(false);
        assert(View(*server.GetLayoutSnapshot(), first_permit.instance).focused);
    }

    wm::WlrServer &server;
    ControlPeer &peer;
    wlr_pointer pointer{};
    wlr_keyboard keyboard{};
    Requests requests;
    std::unique_ptr<ClientProcess> topbar, dock, first, second;
    launch::ShellPermit topbar_permit, dock_permit, first_permit, second_permit;

private:
    std::uint64_t next_identity_{101};
    std::uint32_t time_{1000};
    contracts::LayoutControlRequest last_end_;
    contracts::LayoutControlResult last_result_;
};

} // namespace
