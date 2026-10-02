#include "fixtures/wm_theme_fixture.hpp"
#include "prism/contracts/layout_control.hpp"
#include "prism/launch/control_protocol.hpp"
#include "prism/launch/stream.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/wlr_server.hpp"

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
#include <source_location>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_touch.h>
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
enum class PacketKind : std::uint32_t { Ready, Input };

struct Packet {
    PacketKind kind{};
    contracts::LayoutInputProof proof;
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
        } else if (const auto *touch = std::get_if<contracts::TouchDownEvent>(&event)) {
            Send({PacketKind::Input,
                  {contracts::LayoutInputKind::Touch, touch->protocol_serial, touch->contact}});
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

int RunClient(const char *socket, int channel)
{
    char start{};
    assert(recv(channel, &start, 1, 0) == 1 && start == 'S');
    ClientEvents events(channel);
    platform::WaylandWindow window;
    window.SetPaintHandler(ClientEvents::Paint);
    window.SetEventHandler(std::bind_front(&ClientEvents::Event, &events));
    assert(window.Open(socket, "prism.control-fixture", "Control fixture", 480, 300));
    events.Send({PacketKind::Ready, {}});

    pollfd wake{channel, POLLIN, 0};
    while (window.Pump(5, std::span(&wake, 1))) {
        if (wake.revents & POLLIN) {
            char command{};
            assert(recv(channel, &command, 1, 0) == 1 && command == 'Q');
            break;
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
    explicit ClientProcess(const std::string &socket)
    {
        int channels[2];
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channels) == 0);
        pid = fork();
        assert(pid >= 0);
        if (!pid) {
            close(channels[0]);
            const auto descriptor = std::to_string(channels[1]);
            execl("/proc/self/exe", "native_layout_control_test", "--client", socket.c_str(),
                  descriptor.c_str(), nullptr);
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
            } else {
                assert(packet.kind == PacketKind::Input && packet.proof.serial);
                inputs.push_back(packet.proof);
            }
        }
    }

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
            return messages.size() > previous;
        });
        const auto &reply = messages.back();
        assert(reply.type == launch::ControlType::LayoutControlResult);
        assert(reply.permit.instance == permit.instance && reply.permit.pid == permit.pid);
        assert(reply.control_result.request == request.request);
        assert(reply.control_result.gesture == request.gesture);
        assert(!reply.control_result.applied);
        return reply.control_result;
    }

    contracts::LayoutControlResult Cancelled(wm::WlrServer &server, std::uint64_t session_id)
    {
        Until(server, [&] {
            Collect();
            return std::any_of(messages.begin(), messages.end(), [session_id](const auto &message) {
                return message.type == launch::ControlType::LayoutControlResult &&
                       message.control_result.session == session_id &&
                       message.control_result.status == LayoutControlStatus::Cancelled;
            });
        });
        for (const auto &message : messages) {
            if (message.type == launch::ControlType::LayoutControlResult &&
                message.control_result.session == session_id &&
                message.control_result.status == LayoutControlStatus::Cancelled) {
                assert(!message.control_result.applied);
                return message.control_result;
            }
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

void Check(const contracts::LayoutControlResult &result, LayoutControlStatus status,
           LayoutControlError error = LayoutControlError::None,
           std::source_location location = std::source_location::current())
{
    if (result.status != status || result.error != error || result.applied) {
        std::fprintf(
            stderr,
            "%s:%u: request=%llu session=%llu sequence=%llu "
            "actual status=%u error=%u applied=%u; expected status=%u error=%u applied=0\n",
            location.file_name(), location.line(), static_cast<unsigned long long>(result.request),
            static_cast<unsigned long long>(result.session),
            static_cast<unsigned long long>(result.sequence), static_cast<unsigned>(result.status),
            static_cast<unsigned>(result.error), static_cast<unsigned>(result.applied),
            static_cast<unsigned>(status), static_cast<unsigned>(error));
    }
    assert(result.status == status && result.error == error && !result.applied);
}

void TestControl(wm::WlrServer &server, ControlPeer &peer)
{
    wlr_pointer pointer{};
    static const wlr_pointer_impl pointer_impl{.name = "layout-control-pointer"};
    wlr_pointer_init(&pointer, &pointer_impl, "layout-control-pointer");
    server.HandleNewInput(&pointer.base);
    wlr_touch touch{};
    static const wlr_touch_impl touch_impl{.name = "layout-control-touch"};
    wlr_touch_init(&touch, &touch_impl, "layout-control-touch");
    server.HandleNewInput(&touch.base);

    ClientProcess topbar(server.GetSocketName());
    launch::ShellPermit permit;
    permit.session = peer.session;
    permit.request = {101};
    permit.instance = {201};
    permit.pid = topbar.pid;
    permit.role = contracts::WindowRole::TopBar;
    permit.token.fill(0x51);
    permit.expires_ns = launch::MonotonicNs() + 30'000'000'000ULL;
    launch::ControlMessage grant;
    grant.type = launch::ControlType::Grant;
    grant.permit = permit;
    peer.Send(grant);
    Until(server, [&] { return peer.Has(launch::ControlType::Registered, permit.instance); });
    assert(peer.messages.back().success);
    topbar.Start();
    Until(server, [&] {
        topbar.Collect();
        return topbar.ready && peer.Has(launch::ControlType::Mapped, permit.instance);
    });

    ClientProcess application(server.GetSocketName());
    application.Start();
    Until(server, [&] {
        application.Collect();
        return application.ready && !server.GetLayoutSnapshot()->nodes.empty();
    });
    Requests requests;
    auto proof = Press(server, topbar, &pointer.base);
    auto forged = proof;
    forged.serial ^= 0x40000000;
    Check(peer.Apply(server, permit, requests.Begin(server, forged)), LayoutControlStatus::Rejected,
          LayoutControlError::InvalidInput);
    auto wrong_role = permit;
    wrong_role.role = contracts::WindowRole::Dock;
    Check(peer.Apply(server, wrong_role, requests.Begin(server, proof)),
          LayoutControlStatus::Rejected, LayoutControlError::Unauthorized);

    // Real Up may reach the WM before the asynchronous frontend Begin arrives.
    // A recently released proof remains usable once; it is never synthesized.
    Release(server, &pointer.base);
    auto begin = requests.Begin(server, proof);
    const auto began = peer.Apply(server, permit, begin);
    Check(began, LayoutControlStatus::Began);
    assert(began.session);
    assert(peer.Apply(server, permit, begin) == began);
    auto update = requests.Next(begin, LayoutControlPhase::Update, began.session);
    update.position = {340, 70};
    Check(peer.Apply(server, permit, update), LayoutControlStatus::Updated);
    auto end = requests.Next(update, LayoutControlPhase::End, began.session);
    const auto ended = peer.Apply(server, permit, end);
    Check(ended, LayoutControlStatus::Ended);
    assert(peer.Apply(server, permit, end) == ended);
    Check(peer.Apply(server, permit, requests.Begin(server, proof)), LayoutControlStatus::Rejected,
          LayoutControlError::InvalidInput);

    // Boundary intent remains unsupported; use a real adjacent pair so the
    // rejection tests capability instead of an invalid target identity.
    ClientProcess neighbour(server.GetSocketName());
    neighbour.Start();
    Until(server, [&] {
        neighbour.Collect();
        return neighbour.ready && !server.GetLayoutSnapshot()->boundaries.empty();
    });
    proof = Press(server, topbar, &pointer.base);
    begin = requests.Begin(server, proof);
    begin.operation = contracts::LayoutControlOperation::BoundaryGesture;
    begin.target.boundary = server.GetLayoutSnapshot()->boundaries.front().id;
    const auto boundary = peer.Apply(server, permit, begin);
    Check(boundary, LayoutControlStatus::Began);
    end = requests.Next(begin, LayoutControlPhase::End, boundary.session);
    end.intent = contracts::LayoutControlIntent::ApplyBoundary;
    Check(peer.Apply(server, permit, end), LayoutControlStatus::Rejected,
          LayoutControlError::Unsupported);
    Release(server, &pointer.base);

    proof = Press(server, topbar, &pointer.base);
    begin = requests.Begin(server, proof);
    const auto layout = peer.Apply(server, permit, begin);
    Check(layout, LayoutControlStatus::Began);
    auto theme = test::WmThemeFixture(2);
    theme.layout.inner_gap += 5;
    theme.layout.outer_gap += 3;
    assert(server.InstallTheme(theme).success);
    Check(peer.Cancelled(server, layout.session), LayoutControlStatus::Cancelled,
          LayoutControlError::StaleLayout);
    Release(server, &pointer.base);

    const auto count = topbar.inputs.size();
    const auto output = server.GetLayoutSnapshot()->outputs.front().logical_bounds;
    wlr_touch_down_event down{
        .touch = &touch, .time_msec = 200, .touch_id = 11, .x = 0.5, .y = 20.0 / output.height};
    wl_signal_emit_mutable(&touch.events.down, &down);
    wl_signal_emit_mutable(&touch.events.frame, &touch);
    Until(server, [&] {
        topbar.Collect();
        return topbar.inputs.size() > count;
    });
    const auto touch_proof = topbar.inputs.back();
    assert(touch_proof.kind == contracts::LayoutInputKind::Touch);
    begin = requests.Begin(server, touch_proof);
    const auto touching = peer.Apply(server, permit, begin);
    Check(touching, LayoutControlStatus::Began);
    proof = Press(server, topbar, &pointer.base);
    Check(peer.Apply(server, permit, requests.Begin(server, proof)), LayoutControlStatus::Rejected,
          LayoutControlError::Busy);
    Release(server, &pointer.base);
    wlr_touch_finish(&touch);
    Check(peer.Cancelled(server, touching.session), LayoutControlStatus::Cancelled,
          LayoutControlError::InvalidInput);

    proof = Press(server, topbar, &pointer.base);
    begin = requests.Begin(server, proof);
    const auto unplug = peer.Apply(server, permit, begin);
    Check(unplug, LayoutControlStatus::Began);
    Release(server, &pointer.base);
    wlr_pointer_finish(&pointer);
    Check(peer.Cancelled(server, unplug.session), LayoutControlStatus::Cancelled,
          LayoutControlError::InvalidInput);

    // A new physical device creates a fresh credential. Native surface unmap
    // cancels its session even when the private control connection stays alive.
    wlr_pointer_init(&pointer, &pointer_impl, "layout-control-replacement");
    server.HandleNewInput(&pointer.base);
    proof = Press(server, topbar, &pointer.base);
    begin = requests.Begin(server, proof);
    const auto closing = peer.Apply(server, permit, begin);
    Check(closing, LayoutControlStatus::Began);
    Release(server, &pointer.base);
    topbar.Stop();
    Check(peer.Cancelled(server, closing.session), LayoutControlStatus::Cancelled,
          LayoutControlError::Disconnected);
    Until(server, [&] { return topbar.Reaped(); });
    Check(peer.Apply(server, permit, requests.Begin(server, proof)), LayoutControlStatus::Rejected,
          LayoutControlError::Unauthorized);
    wlr_pointer_finish(&pointer);
    application.Stop();
    neighbour.Stop();
    Until(server, [&] { return application.Reaped() && neighbour.Reaped(); });
    assert(server.ControlHealthy());
}

void RunServer(int server_fd, int peer_fd, const char *directory)
{
    setenv("XDG_RUNTIME_DIR", directory, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    auto compositor = std::make_shared<wm::Compositor>();
    assert(compositor->Initialize());
    wm::WlrServer server(compositor);
    assert(server.Initialize("wayland-control-" + std::to_string(getpid())));
    server.AttachControl(server_fd, getppid());
    server.Start();
    assert(server.InstallTheme(test::WmThemeFixture()).success);

    ControlPeer peer(peer_fd);
    peer.Collect();
    assert(peer.messages.size() == 1 && peer.messages.front().type == launch::ControlType::Ready);
    peer.session = peer.messages.front().permit.session;
    TestControl(server, peer);
    server.Stop();
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--client") {
        return RunClient(argv[2], std::stoi(argv[3]));
    }
    assert(argc == 1);
    char directory[] = "/tmp/prism-native-control.XXXXXX";
    assert(mkdtemp(directory));
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    const auto child = fork();
    assert(child >= 0);
    if (!child) {
        RunServer(sockets[1], sockets[0], directory);
        _exit(0);
    }
    close(sockets[0]);
    close(sockets[1]);
    int status{};
    assert(waitpid(child, &status, 0) == child);
    std::filesystem::remove_all(directory);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::puts("Native layout control: real Shell PID/serial authority, pointer/touch sessions, "
              "replay, unsupported boundary, layout/unplug/unmap cancellation passed");
}
