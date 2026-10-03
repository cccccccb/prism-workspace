#include "fixtures/minimum_size_client.hpp"
#include "fixtures/native_layout_fixture.hpp"

namespace {
class BoundaryFixture {
public:
    explicit BoundaryFixture(Fixture &fixture) : f(fixture)
    {
        client = f.Start(contracts::WindowRole::LayoutControls, permit);
    }

    contracts::LayoutControlRequest Begin()
    {
        const auto snapshot = f.server.GetLayoutSnapshot();
        const auto &boundary = snapshot->boundaries.front();
        assert(boundary.visible && boundary.resizable);
        x = boundary.bounds.x + boundary.bounds.width / 2;
        y = boundary.bounds.y + boundary.bounds.height / 2;
        f.Move(x, y);
        assert(f.server.GetLayoutSnapshot()->control_handle.boundary == boundary.id);
        client->Collect();
        const auto count = client->inputs.size();
        f.Button(true);
        Until(f.server, [&] {
            client->Collect();
            return client->inputs.size() > count;
        });
        auto request = f.requests.Begin(f.server, client->inputs.back());
        request.operation = contracts::LayoutControlOperation::BoundaryGesture;
        request.target.boundary = boundary.id;
        request.position = {24, 24};
        const auto result = f.peer.Apply(f.server, permit, request);
        assert(result.status == LayoutControlStatus::Began && result.session);
        request.session = result.session;
        return request;
    }

    contracts::LayoutControlRequest Update(contracts::LayoutControlRequest request, double delta)
    {
        f.Move(x + delta, y);
        request = f.requests.Next(request, LayoutControlPhase::Update, request.session);
        // Arbitrary but finite local coordinates cannot teleport the divider:
        // the server uses its actual Down/motion/Up seat sample.
        request.position = {900000, -900000};
        const auto result = f.peer.Apply(f.server, permit, request);
        assert(result.status == LayoutControlStatus::Updated && !result.applied);
        assert(result.layout_revision == f.server.GetLayoutSnapshot()->layout_revision);
        assert(f.peer.Apply(f.server, permit, request) == result);
        return request;
    }

    void End(contracts::LayoutControlRequest request, bool cancel = false)
    {
        f.Button(false);
        request =
            f.requests.Next(request, cancel ? LayoutControlPhase::Cancel : LayoutControlPhase::End,
                            request.session);
        request.intent = cancel ? contracts::LayoutControlIntent::None
                                : contracts::LayoutControlIntent::ApplyBoundary;
        const auto result = f.peer.Apply(f.server, permit, request);
        assert(result.status ==
               (cancel ? LayoutControlStatus::Cancelled : LayoutControlStatus::Ended));
        assert(result.applied == !cancel);
        assert(f.peer.Apply(f.server, permit, request) == result);
        f.Settle();
    }

    void Cancelled(std::uint64_t session)
    {
        Until(f.server, [&] {
            f.peer.Collect();
            return std::any_of(f.peer.messages.begin(), f.peer.messages.end(),
                               [session](const auto &m) {
                                   return m.type == launch::ControlType::LayoutControlResult &&
                                          m.control_result.session == session &&
                                          m.control_result.status == LayoutControlStatus::Cancelled;
                               });
        });
    }

    Fixture &f;
    launch::ShellPermit permit;
    std::unique_ptr<ClientProcess> client;
    double x{}, y{};
};

void TestBoundary(wm::WlrServer &server, ControlPeer &peer)
{
    Fixture f(server, peer);
    BoundaryFixture b(f);
    const auto initial = server.GetLayoutSnapshot();
    const auto width = View(*initial, f.first_permit.instance).target_bounds.width;
    const auto &gap = initial->boundaries.front().bounds;
    const auto before_buttons = f.Sync(*f.second).buttons;
    f.Move(gap.x + gap.width / 2 + 20, gap.y + gap.height / 2);
    assert(server.GetLayoutSnapshot()->control_handle.visible);
    f.Button(true);
    f.Button(false);
    assert(f.Sync(*f.second).buttons == before_buttons + 2);
    b.client->Collect();
    assert(b.client->inputs.empty()); // A full client input region cannot cover adjacent apps.

    auto request = b.Begin();
    request = b.Update(request, 80);
    assert(View(*server.GetLayoutSnapshot(), f.first_permit.instance).target_bounds.width ==
           width + 80);
    b.End(request);

    const auto accepted = server.GetLayoutSnapshot();
    request = b.Begin();
    request = b.Update(request, -40);
    b.End(request, true);
    SameTree(*accepted, *server.GetLayoutSnapshot());
    assert(View(*server.GetLayoutSnapshot(), f.first_permit.instance).target_bounds ==
           View(*accepted, f.first_permit.instance).target_bounds);

    // A stuck application cannot accumulate one configure per pointer sample.
    const auto count = f.Sync(*f.first).configures;
    assert(kill(f.first->pid, SIGSTOP) == 0);
    int status{};
    assert(waitpid(f.first->pid, &status, WUNTRACED) == f.first->pid && WIFSTOPPED(status));
    request = b.Begin();
    const auto old_width =
        View(*server.GetLayoutSnapshot(), f.first_permit.instance).target_bounds.width;
    for (int i = 1; i <= 20; ++i) {
        request = b.Update(request, -i * 3);
    }
    const auto preview = server.GetLayoutSnapshot();
    const auto &slow = View(*preview, f.first_permit.instance);
    assert(slow.target_bounds.width == old_width - 60);
    assert(slow.committed_bounds.width == old_width);
    assert(kill(f.first->pid, SIGCONT) == 0);
    b.End(request);
    const auto after_count = f.Sync(*f.first).configures;
    assert(after_count >= count + 1 && after_count <= count + 2);

    // Theme/layout change cancels and restores only this pair's preview.
    const auto before_theme = server.GetLayoutSnapshot();
    request = b.Begin();
    request = b.Update(request, 30);
    auto theme = test::WmThemeFixture(2);
    theme.layout.outer_gap += 2;
    assert(server.InstallTheme(theme).success);
    b.Cancelled(request.session);
    f.Button(false);
    f.Settle();
    SameTree(*before_theme, *server.GetLayoutSnapshot());

    // Lost input device rolls back; no synthetic End commits the preview.
    const auto before_loss = server.GetLayoutSnapshot();
    request = b.Begin();
    request = b.Update(request, 20);
    wlr_pointer_finish(&f.pointer);
    b.Cancelled(request.session);
    SameTree(*before_loss, *server.GetLayoutSnapshot());
    static const wlr_pointer_impl impl{.name = "replacement-boundary-pointer"};
    wlr_pointer_init(&f.pointer, &impl, "replacement-boundary-pointer");
    server.HandleNewInput(&f.pointer.base);

    // Role isolation is enforced by WM even on its private control endpoint.
    auto denied = f.requests.Begin(server, {});
    denied.input = {contracts::LayoutInputKind::Pointer, 1, 0};
    denied.operation = contracts::LayoutControlOperation::GroupGesture;
    assert(peer.Apply(server, b.permit, denied).error == LayoutControlError::Unauthorized);

    // Committed XDG minimum sizes flow into the tree, including changes mid-drag.
    launch::ShellPermit minimum_permit;
    auto minimum = f.Start(contracts::WindowRole::Toplevel, minimum_permit, true);
    f.Settle();
    const auto &windows = server.GetCompositor()->GetWindows();
    const auto minimum_window =
        std::find_if(windows.begin(), windows.end(), [&](const auto &window) {
            return window->GetInstance() == minimum_permit.instance.value;
        });
    assert(minimum_window != windows.end());
    assert((*minimum_window)->GetMinimumWidth() == 240 &&
           (*minimum_window)->GetMinimumHeight() == 120);
    request = b.Begin();
    request = b.Update(request, 10);
    minimum->Minimum();
    b.Cancelled(request.session);
    f.Button(false);
    assert((*minimum_window)->GetMinimumWidth() == 2000);
    minimum->Stop();
    Until(server, [&] { return minimum->Reaped(); });
    f.Settle();

    // Removing a neighbour invalidates topology. Cancellation must not revive it.
    request = b.Begin();
    request = b.Update(request, 20);
    f.second->Stop();
    Until(server, [&] { return f.second->Reaped(); });
    b.Cancelled(request.session);
    f.Button(false);
    assert(server.GetCompositor()->GetWindows().size() == 1);
    assert(server.GetLayoutSnapshot()->boundaries.empty());
    assert(!server.GetLayoutSnapshot()->control_handle.visible);
}

void RunServer(int server_fd, int peer_fd, const char *directory)
{
    setenv("XDG_RUNTIME_DIR", directory, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("XKB_DEFAULT_LAYOUT", "us", 1);
    auto compositor = std::make_shared<wm::Compositor>();
    assert(compositor->Initialize());
    wm::WlrServer server(compositor);
    assert(server.Initialize("wayland-boundary-" + std::to_string(getpid())));
    server.AttachControl(server_fd, getppid());
    server.Start();
    assert(server.InstallTheme(test::WmThemeFixture()).success);
    ControlPeer peer(peer_fd);
    peer.Collect();
    assert(peer.messages.size() == 1 && peer.messages.front().type == launch::ControlType::Ready);
    peer.session = peer.messages.front().permit.session;
    TestBoundary(server, peer);
    server.Stop();
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--client") {
        return RunClient(argv[2], std::stoi(argv[3]));
    }
    if (argc == 4 && std::string_view(argv[1]) == "--minimum-client") {
        return MinimumSizeClient(std::stoi(argv[3])).Run(argv[2]);
    }
    assert(argc == 1);
    char directory[] = "/tmp/prism-native-boundary.XXXXXX";
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
    std::puts("Native boundary: mouse authority, live preview, cancel, configure backpressure, "
              "role isolation and topology cleanup passed");
}
