#include "../prism/src/wm/wlr_group_geometry.hpp"
#include "../prism/src/wm/wlr_surface_geometry.hpp"
#include "fixtures/minimum_size_client.hpp"
#include "fixtures/native_layout_fixture.hpp"
#include "prism/wm/xdg_view.hpp"
#include <cmath>
#include <linux/input-event-codes.h>
extern "C" {
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
}

namespace {
wlr_scene_surface *Find(wlr_scene_node *node, pid_t wanted)
{
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        auto *surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        if (surface) {
            pid_t pid{};
            wl_client_get_credentials(wl_resource_get_client(surface->surface->resource), &pid,
                                      nullptr, nullptr);
            if (pid == wanted) {
                return surface;
            }
        }
    } else if (node->type == WLR_SCENE_NODE_TREE) {
        wlr_scene_node *child;
        wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link)
        {
            if (auto *found = Find(child, wanted)) {
                return found;
            }
        }
    }
    return nullptr;
}

struct Frame {
    contracts::LogicalRect bounds;
    int source_width{}, source_height{};
};

struct Frames {
    wl_listener listener{};
    wm::WlrServer *server;
    pid_t pid;
    std::vector<Frame> values;

    Frames(wm::WlrServer &server, pid_t pid) : server(&server), pid(pid)
    {
        wl_list_init(&listener.link);
        wlr_scene_output *output;
        wl_list_for_each(output, &server.GetScene()->outputs, link)
        {
            listener.notify = Commit;
            wl_signal_add(&output->output->events.commit, &listener);
            break;
        }
    }

    ~Frames()
    {
        wl_list_remove(&listener.link);
    }

    static void Commit(wl_listener *listener, void *data)
    {
        auto *self = reinterpret_cast<Frames *>(listener);
        const auto *event = static_cast<wlr_output_event_commit *>(data);
        if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER)) {
            return;
        }
        auto *surface = Find(&self->server->GetScene()->tree.node, self->pid);
        if (!surface) {
            return;
        }
        auto *buffer = surface->buffer;
        int x{}, y{};
        if (!wlr_scene_node_coords(&buffer->node, &x, &y)) {
            return;
        }
        const auto &current = surface->surface->current;
        self->values.push_back(
            {{double(x), double(y), double(buffer->dst_width ? buffer->dst_width : current.width),
              double(buffer->dst_height ? buffer->dst_height : current.height)},
             current.width,
             current.height});
    }
};

void PumpFor(wm::WlrServer &server, std::chrono::milliseconds duration)
{
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        server.RunEventLoopIteration(5);
    }
}

void CheckFailedCandidate(Fixture &f)
{
    auto *surface = Find(&f.server.GetScene()->tree.node, f.first->pid)->surface;
    auto *scene = wlr_scene_create();
    {
        wm::WlrXdgView view;
        view.scene_tree = wlr_scene_tree_create(&scene->tree);
        view.toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);
        view.mapped = view.visible = true;
        view.x = 20;
        view.y = 30;
        view.width = 800;
        view.height = 600;
        const float color[4]{1, 0, 0, 1};
        auto *rect = wlr_scene_rect_create(view.scene_tree, 200, 100, color);
        wm::SurfaceGeometry motion(&view);
        motion.Start({10, 10, 200, 100}, {20, 30, 800, 600}, {}, {},
                     {"window.geometry", 0, contracts::MotionEasing::Linear});
        const auto before = motion.Submitted();
        assert(motion.Prepare());
        assert(!motion.SubmittedFrame(false));
        assert(motion.Submitted() == before && motion.NeedsFrame());
        assert(motion.Prepare());
        motion.SubmittedFrame(true);
        assert(motion.Submitted() == (contracts::LogicalRect{20, 30, 800, 600}));
        assert(!motion.NeedsFrame());
        // Destroying a transformed node invalidates its saved restore record.
        wlr_scene_node_destroy(&rect->node);
        motion.Restore();
    }
    wlr_scene_node_destroy(&scene->tree.node);
}

void TestGroupMotion(Fixture &f)
{
    auto &server = f.server;
    const auto before = server.GetLayoutSnapshot();
    const auto graph = wm::BuildGroupGeometry(*before);
    assert(graph && graph->members.size() == 2);
    const auto a = View(*before, f.first_permit.instance).target_bounds;
    const auto b = View(*before, f.second_permit.instance).target_bounds;
    Frames first(server, f.first->pid), second(server, f.second->pid);
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    const auto target = View(*server.GetLayoutSnapshot(), f.first_permit.instance).target_bounds;
    assert(target.height > a.height);
    Until(server, [&] {
        return !first.values.empty() && first.values.back().bounds.height > a.height + 2 &&
               first.values.back().bounds.height < target.height - 2;
    });
    assert(first.values.size() == second.values.size());
    for (std::size_t i = 0; i < first.values.size(); ++i) {
        const auto &x = first.values[i].bounds;
        const auto &y = second.values[i].bounds;
        assert(x.y == y.y && x.height == y.height);
        assert(y.x - x.x - x.width == b.x - a.x - a.width);
    }
    const auto sample = second.values.back();
    f.Move(sample.bounds.x + sample.bounds.width / 2, sample.bounds.y + sample.bounds.height / 2);
    auto *expected = Find(&server.GetScene()->tree.node, f.second->pid)->surface;
    assert(server.GetSeat()->pointer_state.focused_surface == expected);
    assert(std::abs(server.GetSeat()->pointer_state.sy - sample.source_height / 2.0) < 2);
    const auto first_sample = first.values.back();
    f.Move(first_sample.bounds.x + first_sample.bounds.width / 2,
           first_sample.bounds.y + first_sample.bounds.height / 2);
    f.Button(true);
    auto *grabbed = Find(&server.GetScene()->tree.node, f.first->pid)->surface;
    f.Move(first_sample.bounds.x + first_sample.bounds.width + 20,
           first_sample.bounds.y + first_sample.bounds.height / 2);
    assert(server.GetSeat()->pointer_state.focused_surface == grabbed);
    assert(server.GetSeat()->pointer_state.sx > first_sample.source_width);
    f.Button(false);
    const auto count_a = f.Sync(*f.first).configures;
    const auto count_b = f.Sync(*f.second).configures;
    PumpFor(server, 30ms);
    assert(f.Sync(*f.first).configures == count_a && f.Sync(*f.second).configures == count_b);

    // Restore the whole group while it is still moving, without a client gesture timeout.
    f.Key(KEY_F, true); // Super+Shift+F restores the group.
    Until(server,
          [&] { return first.values.back().bounds == a && second.values.back().bounds == b; });
    PumpFor(server, 100ms);
    const auto idle = server.GetFrameCount();
    PumpFor(server, 100ms);
    assert(server.GetFrameCount() == idle);

    // Workspace switch cancels all member adapters together.
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.Key(KEY_2);
    f.Key(KEY_1);
    f.Key(KEY_F, true);
    f.Settle();
}

void TestMotion(wm::WlrServer &server, ControlPeer &peer)
{
    Fixture f(server, peer);
    f.FocusFirst();
    CheckFailedCandidate(f);
    auto theme = test::WmThemeFixture(2);
    theme.schema_version = 3;
    theme.motion = {"fixture",
                    {{"panel.visibility", 0, contracts::MotionEasing::Linear},
                     {"window.geometry", 500, contracts::MotionEasing::Linear},
                     {"group.geometry", 500, contracts::MotionEasing::Linear}}};
    assert(server.InstallTheme(theme).success);
    TestGroupMotion(f);
    f.FocusFirst();
    Frames frames(server, f.first->pid);
    const auto original = View(*server.GetLayoutSnapshot(), f.first_permit.instance).target_bounds;
    f.Key(KEY_F);
    const auto target = View(*server.GetLayoutSnapshot(), f.first_permit.instance).target_bounds;
    assert(target.width > original.width);
    Until(server, [&] {
        return !frames.values.empty() && frames.values.back().bounds.width > original.width + 10 &&
               frames.values.back().bounds.width < target.width - 10;
    });
    const auto sample = frames.values.back();
    f.Move(sample.bounds.x + sample.bounds.width / 2, sample.bounds.y + sample.bounds.height / 2);
    auto *expected = Find(&server.GetScene()->tree.node, f.first->pid)->surface;
    assert(server.GetSeat()->pointer_state.focused_surface == expected);
    assert(std::abs(server.GetSeat()->pointer_state.sx - sample.source_width / 2.0) < 2);
    assert(std::abs(server.GetSeat()->pointer_state.sy - sample.source_height / 2.0) < 2);
    f.Button(true);
    f.Move(sample.bounds.x + sample.bounds.width + 10, sample.bounds.y + sample.bounds.height / 2);
    assert(server.GetSeat()->pointer_state.focused_surface == expected);
    assert(server.GetSeat()->pointer_state.sx > sample.source_width);
    f.Button(false);
    const auto configures = f.Sync(*f.first).configures;
    PumpFor(server, 60ms);
    assert(f.Sync(*f.first).configures == configures);

    const auto reverse_from = frames.values.back().bounds;
    f.Key(KEY_F);
    assert(View(*server.GetLayoutSnapshot(), f.first_permit.instance).target_bounds == original);
    Until(server, [&] { return frames.values.back().bounds.width < reverse_from.width - 5; });
    Until(server, [&] { return frames.values.back().bounds == original; });
    PumpFor(server, 100ms);
    const auto idle = server.GetFrameCount();
    PumpFor(server, 100ms);
    assert(server.GetFrameCount() == idle);
    std::printf("Geometry: intermediate %.0fx%.0f, inverse input and reverse passed\n",
                sample.bounds.width, sample.bounds.height);

    f.Key(KEY_F);
    f.Key(KEY_2);
    assert(!View(*server.GetLayoutSnapshot(), f.first_permit.instance).visible);
    f.Key(KEY_1);
    theme.generation = 3;
    theme.motion.transitions[1].duration_ms = 0;
    assert(server.InstallTheme(theme).success);
    f.Key(KEY_F);
    f.Settle();
    assert(!View(*server.GetLayoutSnapshot(), f.first_permit.instance).fullscreen);
    theme.generation = 4;
    theme.motion.transitions[1].duration_ms = 500;
    assert(server.InstallTheme(theme).success);
    f.Key(KEY_F);
    f.first->Stop();
    Until(server, [&] { return f.first->Reaped(); });
}

void RunServer(int server_fd, int peer_fd, const char *directory)
{
    setenv("XDG_RUNTIME_DIR", directory, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", std::getenv("PRISM_TEST_GLES") ? "gles2" : "pixman", 1);
    setenv("XKB_DEFAULT_LAYOUT", "us", 1);
    auto compositor = std::make_shared<wm::Compositor>();
    assert(compositor->Initialize());
    wm::WlrServer server(compositor);
    assert(server.Initialize("wayland-geometry-" + std::to_string(getpid())));
    server.AttachControl(server_fd, getppid());
    server.Start();
    assert(server.InstallTheme(test::WmThemeFixture()).success);
    ControlPeer peer(peer_fd);
    peer.Collect();
    peer.session = peer.messages.front().permit.session;
    TestMotion(server, peer);
    server.Stop();
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--client") {
        return RunClient(argv[2], std::stoi(argv[3]), std::getenv("PRISM_TEST_GLES") != nullptr);
    }
    if (argc == 4 && std::string_view(argv[1]) == "--minimum-client") {
        return MinimumSizeClient(std::stoi(argv[3])).Run(argv[2]);
    }
    char directory[] = "/tmp/prism-native-geometry.XXXXXX";
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
}
