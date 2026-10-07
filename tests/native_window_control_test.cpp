#include "fixtures/minimum_size_client.hpp"
#include "fixtures/native_layout_fixture.hpp"
#include <linux/input-event-codes.h>
extern "C" {
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
}

namespace {
class WindowFixture {
public:
    explicit WindowFixture(Fixture &fixture) : f(fixture)
    {
        client = f.Start(contracts::WindowRole::LayoutControls, permit);
    }

    void Open()
    {
        const auto snapshot = f.server.GetLayoutSnapshot();
        const auto &view = View(*snapshot, f.first_permit.instance);
        const auto bounds = view.target_bounds;
        f.Move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
        const auto before = f.Sync(*f.first).buttons;
        const auto logo = xkb_keymap_mod_get_index(f.keyboard.keymap, XKB_MOD_NAME_LOGO);
        wlr_keyboard_notify_modifiers(&f.keyboard, 1U << logo, 0, 0, 0);
        f.server.HandleCursorButton(100, 273, WL_POINTER_BUTTON_STATE_PRESSED, &f.pointer.base);
        f.server.HandleCursorButton(101, 273, WL_POINTER_BUTTON_STATE_RELEASED, &f.pointer.base);
        wlr_keyboard_notify_modifiers(&f.keyboard, 0, 0, 0, 0);
        assert(f.Sync(*f.first).buttons == before);
        handle = f.server.GetLayoutSnapshot()->control_handle;
        assert(handle.visible && handle.node == view.id && !handle.boundary);
        assert(handle.bounds.width == 168 && handle.bounds.height == 56);
        f.Sync(*client);
        f.Sync(*client);
    }

    contracts::LayoutControlRequest Begin(int slot)
    {
        f.Move(handle.bounds.x + 32 + slot * 52, handle.bounds.y + 28);
        client->Collect();
        const auto count = client->inputs.size();
        f.Button(true);
        Until(f.server, [&] {
            client->Collect();
            return client->inputs.size() > count;
        });
        auto request = f.requests.Begin(f.server, client->inputs.back());
        request.operation = contracts::LayoutControlOperation::WindowGesture;
        request.target.node = handle.node;
        request.position = {32.0 + slot * 52, 28};
        const auto result = f.peer.Apply(f.server, permit, request);
        assert(result.status == LayoutControlStatus::Began && result.session);
        request.session = result.session;
        return request;
    }

    void End(contracts::LayoutControlRequest request, contracts::LayoutControlIntent intent,
             LayoutControlError expected = LayoutControlError::None)
    {
        f.Button(false);
        request = f.requests.Next(request, LayoutControlPhase::End, request.session);
        request.intent = intent;
        const auto result = f.peer.Apply(f.server, permit, request);
        assert(result.error == expected);
        assert(result.applied == (expected == LayoutControlError::None));
        assert(f.peer.Apply(f.server, permit, request) == result);
        f.Settle();
    }

    Fixture &f;
    launch::ShellPermit permit;
    std::unique_ptr<ClientProcess> client;
    contracts::LayoutControlHandle handle;
    std::vector<wlr_scene_buffer_point_accepts_input_func_t> effect_callbacks;
};

struct FadeNodes {
    float live_opacity{-1};
    unsigned ghosts{};
};

void InspectFade(wlr_scene_node *node, pid_t pid, FadeNodes &result,
                 std::vector<wlr_scene_buffer_point_accepts_input_func_t> &effects,
                 bool collect = false)
{
    if (!node->enabled) {
        return;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        auto *tree = wlr_scene_tree_from_node(node);
        wlr_scene_node *child;
        wl_list_for_each(child, &tree->children, link)
        {
            InspectFade(child, pid, result, effects, collect);
        }
    } else if (node->type == WLR_SCENE_NODE_BUFFER) {
        auto *buffer = wlr_scene_buffer_from_node(node);
        auto *surface = wlr_scene_surface_try_from_buffer(buffer);
        if (surface) {
            pid_t owner{};
            wl_client_get_credentials(wl_resource_get_client(surface->surface->resource), &owner,
                                      nullptr, nullptr);
            if (owner == pid) {
                result.live_opacity = buffer->opacity;
            }
        } else if (collect) {
            if (std::find(effects.begin(), effects.end(), buffer->point_accepts_input) ==
                effects.end()) {
                effects.push_back(buffer->point_accepts_input);
            }
        } else if (std::find(effects.begin(), effects.end(), buffer->point_accepts_input) ==
                   effects.end()) {
            ++result.ghosts;
            double x{}, y{};
            assert(buffer->point_accepts_input && !buffer->point_accepts_input(buffer, &x, &y));
        }
    }
}

FadeNodes FadeState(WindowFixture &w)
{
    FadeNodes result;
    InspectFade(&w.f.server.GetScene()->tree.node, w.client->pid, result, w.effect_callbacks);
    return result;
}

void PumpFor(wm::WlrServer &server, std::chrono::milliseconds duration)
{
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        server.RunEventLoopIteration(5);
    }
}

void TestFade(WindowFixture &w)
{
    auto &f = w.f;
    auto theme = *f.server.GetTheme();
    theme.schema_version = 3;
    ++theme.generation;
    theme.motion = {"fixture", {{"panel.visibility", 400, contracts::MotionEasing::Linear}}};
    assert(f.server.InstallTheme(theme).success);
    if (std::getenv("PRISM_TEST_GLES")) {
        Until(f.server, [&] {
            FadeNodes ignored;
            InspectFade(&f.server.GetScene()->tree.node, w.client->pid, ignored, w.effect_callbacks,
                        true);
            return !w.effect_callbacks.empty();
        });
    }
    w.Open();
    Until(f.server, [&] {
        const auto alpha = FadeState(w).live_opacity;
        return alpha > 0.1f && alpha < 0.9f;
    });
    const auto before = FadeState(w).live_opacity;
    f.Key(KEY_ESC);
    assert(!f.server.GetLayoutSnapshot()->control_handle.visible);
    assert(FadeState(w).ghosts >= (std::getenv("PRISM_TEST_GLES") ? 2U : 1U) &&
           FadeState(w).live_opacity < 0);
    // Reopen at the identical location while the visual snapshot is alive.
    w.Open();
    // A newly committed scene buffer gets its inherited opacity at the next
    // output frame; inspect the adapter's sample, not that unpresented buffer.
    Until(f.server, [&] {
        const auto alpha = FadeState(w).live_opacity;
        return alpha > 0 && alpha < 0.9f;
    });
    const auto reopened = FadeState(w);
    std::fprintf(stderr, "fade reverse: before=%.3f after=%.3f ghosts=%u bounds=%.0f,%.0f\n",
                 before, reopened.live_opacity, reopened.ghosts, w.handle.bounds.x,
                 w.handle.bounds.y);
    assert(reopened.ghosts == 0 && reopened.live_opacity > 0 &&
           reopened.live_opacity <= before + 0.2f);
    Until(f.server, [&] { return FadeState(w).live_opacity == 1; });
    const auto configures = f.Sync(*w.client).configures;
    f.Key(KEY_ESC);
    assert(FadeState(w).ghosts > 0);
    // The snapshot cannot intercept a new click inside the former panel bounds.
    f.Move(w.handle.bounds.x + 32, w.handle.bounds.y + 28);
    const auto buttons = f.Sync(*f.first).buttons;
    f.Button(true);
    f.Button(false);
    assert(f.Sync(*f.first).buttons == buttons + 2);
    Until(f.server, [&] { return FadeState(w).ghosts == 0; });
    // Switching the shared role back to its 48x48 divider mode may configure once.
    assert(f.Sync(*w.client).configures <= configures + 1);
    PumpFor(f.server, 100ms);
    const auto idle_frames = f.server.GetFrameCount();
    PumpFor(f.server, 100ms);
    assert(f.server.GetFrameCount() == idle_frames);

    // An accepted theme replacement destroys a still-fading snapshot immediately.
    w.Open();
    Until(f.server, [&] {
        const auto alpha = FadeState(w).live_opacity;
        return alpha > 0 && alpha < 0.9f;
    });
    Until(f.server, [&] { return FadeState(w).live_opacity == 1; });
    f.Key(KEY_ESC);
    assert(FadeState(w).ghosts > 0);
    auto invalid = theme;
    ++invalid.generation;
    invalid.motion.transitions.front().name = "missing";
    assert(!f.server.InstallTheme(invalid).success && FadeState(w).ghosts > 0);

    ++theme.generation;
    theme.motion.transitions.front().duration_ms = 0;
    assert(f.server.InstallTheme(theme).success && FadeState(w).ghosts == 0);
    w.Open();
    assert(FadeState(w).live_opacity == 1);
    f.Key(KEY_ESC);
    assert(FadeState(w).ghosts == 0);
}

void TestWindow(wm::WlrServer &server, ControlPeer &peer)
{
    Fixture f(server, peer);
    WindowFixture w(f);
    TestFade(w);
    const auto original = server.GetLayoutSnapshot();
    using Intent = contracts::LayoutControlIntent;
    w.Open();
    auto request = w.Begin(0);
    w.End(request, Intent::EnterWindowFullscreen);
    assert(View(*server.GetLayoutSnapshot(), f.first_permit.instance).fullscreen);
    assert(!server.GetLayoutSnapshot()->control_handle.node);
    w.Open();
    request = w.Begin(0);
    w.End(request, Intent::ExitWindowFullscreen);
    assert(!View(*server.GetLayoutSnapshot(), f.first_permit.instance).fullscreen);
    SameTree(*original, *server.GetLayoutSnapshot());

    w.Open();
    request = w.Begin(2);
    w.End(request, Intent::SplitVertical);
    const auto vertical = server.GetLayoutSnapshot();
    assert(vertical->nodes.front().layout == contracts::LayoutArrangement::Vertical);
    const auto first = View(*vertical, f.first_permit.instance).target_bounds;
    const auto second = View(*vertical, f.second_permit.instance).target_bounds;
    assert(first.y != second.y && first.x == second.x);
    w.Open();
    request = w.Begin(1);
    w.End(request, Intent::SplitHorizontal);
    const auto horizontal = server.GetLayoutSnapshot();
    assert(horizontal->nodes.front().layout == contracts::LayoutArrangement::Horizontal);
    for (std::size_t i = 0; i < original->nodes.size(); ++i) {
        assert(original->nodes[i].id == horizontal->nodes[i].id);
        assert(original->nodes[i].width_fraction == horizontal->nodes[i].width_fraction);
        assert(original->nodes[i].height_fraction == horizontal->nodes[i].height_fraction);
    }

    // Closing click must not leak either edge to the underlying app.
    w.Open();
    f.Move(w.handle.bounds.x - 20, w.handle.bounds.y);
    const auto buttons = f.Sync(*f.first).buttons;
    f.Button(true);
    f.Button(false);
    assert(!server.GetLayoutSnapshot()->control_handle.node);
    assert(f.Sync(*f.first).buttons == buttons);
    w.Open();
    f.Key(KEY_ESC);
    assert(!server.GetLayoutSnapshot()->control_handle.node);

    // End cannot execute while the real pointer is still held down.
    w.Open();
    request = w.Begin(0);
    auto early = f.requests.Next(request, LayoutControlPhase::End, request.session);
    early.intent = Intent::EnterWindowFullscreen;
    assert(peer.Apply(server, w.permit, early).error == LayoutControlError::InvalidInput);
    f.Button(false);
    f.Key(KEY_ESC);
    assert(!View(*server.GetLayoutSnapshot(), f.first_permit.instance).fullscreen);

    w.Open();
    request = w.Begin(0);
    f.Move(w.handle.bounds.x - 20, w.handle.bounds.y);
    w.End(request, Intent::EnterWindowFullscreen, LayoutControlError::InvalidInput);
    f.Key(KEY_ESC);

    // Topbar cannot use the private window role, even with a valid control proof.
    w.Open();
    request = w.Begin(0);
    auto denied = request;
    denied.request += 10000;
    denied.session = 0;
    assert(peer.Apply(server, f.topbar_permit, denied).error == LayoutControlError::Unauthorized);
    f.Key(KEY_ESC);
    f.Button(false);
    assert(!server.GetLayoutSnapshot()->control_handle.node);

    // Device disappearance while idle in a palette must hide it too.
    w.Open();
    wlr_pointer_finish(&f.pointer);
    assert(!server.GetLayoutSnapshot()->control_handle.node);
    static const wlr_pointer_impl impl{.name = "replacement-window-pointer"};
    wlr_pointer_init(&f.pointer, &impl, "replacement-window-pointer");
    server.HandleNewInput(&f.pointer.base);

    w.Open();
    f.Key(KEY_2);
    assert(!server.GetLayoutSnapshot()->control_handle.node);
    f.Key(KEY_1);
    w.Open();
    f.first->Stop();
    // Process exit does not acknowledge dispatch of its Wayland disconnect.
    // Observe the owner leaving the server tree before checking control cleanup.
    Until(server, [&] {
        const auto snapshot = server.GetLayoutSnapshot();
        return f.first->Reaped() &&
               std::none_of(snapshot->nodes.begin(), snapshot->nodes.end(), [&f](const auto &node) {
                   return node.kind == contracts::LayoutNodeKind::View &&
                          node.instance == f.first_permit.instance;
               });
    });
    assert(!server.GetLayoutSnapshot()->control_handle.node);
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
    assert(server.Initialize("wayland-window-" + std::to_string(getpid())));
    server.AttachControl(server_fd, getppid());
    server.Start();
    assert(server.InstallTheme(test::WmThemeFixture()).success);
    ControlPeer peer(peer_fd);
    peer.Collect();
    assert(peer.messages.size() == 1 && peer.messages.front().type == launch::ControlType::Ready);
    peer.session = peer.messages.front().permit.session;
    TestWindow(server, peer);
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
    assert(argc == 1);
    char directory[] = "/tmp/prism-native-window.XXXXXX";
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
    std::puts(
        "Native window controls: fullscreen, restore, split, input proofs and cancellation passed");
}
