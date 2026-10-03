#include "fixtures/native_layout_fixture.hpp"

namespace {
void TestModeAndOverlay(Fixture &f)
{
    auto &tree = f.server.GetCompositor()->GetTreeEngine();
    const auto &windows = f.server.GetCompositor()->GetWindows();
    assert(windows.size() == 2);
    auto first_node = tree.FindViewForWindow(windows[0]);
    auto second_node = tree.FindViewForWindow(windows[1]);
    assert(first_node && second_node);
    assert(first_node->SetFractions(0.3, 1.0));
    assert(second_node->SetFractions(0.7, 1.0));
    assert(f.server.InstallTheme(test::WmThemeFixture(2)).success);
    f.Settle();

    const auto normal = f.server.GetLayoutSnapshot();
    assert(Active(*normal).mode == contracts::LayoutGroupMode::Normal);
    const auto normal_revision = Active(*normal).mode_revision;
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.Mode(contracts::LayoutGroupMode::Immersive);
    const auto immersive = f.server.GetLayoutSnapshot();
    SameTree(*normal, *immersive);
    assert(Active(*immersive).mode_revision > normal_revision);
    for (const auto instance : {f.first_permit.instance, f.second_permit.instance}) {
        const auto &node = View(*immersive, instance);
        const auto &before = View(*normal, instance);
        assert(node.visible && !node.fullscreen);
        assert(node.target_bounds.height > before.target_bounds.height);
        assert(node.target_bounds.y < before.target_bounds.y);
    }

    // Focus-only arrangement must not forget a completed control just because
    // the originating Topbar is now hidden by the accepted immersive mode.
    f.FocusFirst();
    f.Key(106); // Super+Right arranges the same geometry while changing focus.
    f.ReplayEnd();
    assert(f.server.GetLayoutSnapshot()->layout_revision == immersive->layout_revision);

    // Super+F is a separate XDG overlay. Removing it returns to the existing
    // immersive group without incrementing its explicit mode revision.
    f.FocusFirst();
    f.Key(33);
    const auto covered = f.server.GetLayoutSnapshot();
    assert(View(*covered, f.first_permit.instance).fullscreen);
    assert(!View(*covered, f.second_permit.instance).visible);
    assert(Active(*covered).mode_revision == Active(*immersive).mode_revision);
    f.Key(33);
    f.Mode(contracts::LayoutGroupMode::Immersive);
    assert(View(*f.server.GetLayoutSnapshot(), f.second_permit.instance).visible);
    SameTree(*normal, *f.server.GetLayoutSnapshot());

    // Explicit Exit clears the overlay and changes group mode. A subsequent
    // Super+F on/off pair cannot resurrect the older immersive mode.
    f.Key(33);
    f.Reveal();
    f.Transition(contracts::LayoutControlIntent::ExitImmersive);
    f.Mode(contracts::LayoutGroupMode::Normal);
    assert(!View(*f.server.GetLayoutSnapshot(), f.first_permit.instance).fullscreen);
    assert(View(*f.server.GetLayoutSnapshot(), f.second_permit.instance).visible);
    f.FocusFirst();
    f.Key(33);
    f.Key(33);
    f.Mode(contracts::LayoutGroupMode::Normal);
    SameTree(*normal, *f.server.GetLayoutSnapshot());

    // Enter requested while a single-window overlay is active updates the
    // underlying group mode only; the overlay remains until Super+F removes it.
    f.Key(33);
    f.Reveal();
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    assert(View(*f.server.GetLayoutSnapshot(), f.first_permit.instance).fullscreen);
    assert(!View(*f.server.GetLayoutSnapshot(), f.second_permit.instance).visible);
    f.Key(33);
    f.Mode(contracts::LayoutGroupMode::Immersive);
    assert(View(*f.server.GetLayoutSnapshot(), f.second_permit.instance).visible);
    f.Reveal();
    f.Transition(contracts::LayoutControlIntent::ExitImmersive);
    f.Mode(contracts::LayoutGroupMode::Normal);
}

void TestThemeAndConfigure(Fixture &f)
{
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    const auto before = f.server.GetLayoutSnapshot();
    auto theme = test::WmThemeFixture(3);
    theme.layout.topbar_surface_height = 64;
    theme.layout.dock_surface_height = 90;
    theme.layout.outer_gap = 22;
    theme.layout.inner_gap = 18;
    assert(f.server.InstallTheme(theme).success);
    f.Settle();
    const auto themed = f.server.GetLayoutSnapshot();
    SameTree(*before, *themed);
    assert(Active(*themed).mode == contracts::LayoutGroupMode::Immersive);
    assert(Active(*themed).mode_revision == Active(*before).mode_revision);
    assert(themed->layout_revision > before->layout_revision);

    const auto first = f.Sync(*f.first);
    const auto second = f.Sync(*f.second);
    f.Reveal();
    assert(f.Sync(*f.first).configures == first.configures);
    assert(f.Sync(*f.second).configures == second.configures);
    for (int turn = 0; turn < 4; ++turn) {
        f.server.RunEventLoopIteration(5);
    }
    assert(f.Sync(*f.first).configures == first.configures);
    assert(f.Sync(*f.second).configures == second.configures);

    f.Transition(contracts::LayoutControlIntent::ExitImmersive);
    const auto restored = f.server.GetLayoutSnapshot();
    SameTree(*themed, *restored);
    for (const auto instance : {f.first_permit.instance, f.second_permit.instance}) {
        const auto &bounds = View(*restored, instance).target_bounds;
        assert(bounds.y >= theme.layout.topbar_surface_height);
        assert(bounds.y > View(*themed, instance).target_bounds.y);
    }
}

void TestMouseAndKeyboardRecovery(Fixture &f)
{
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    const auto first_before = f.Sync(*f.first);
    const auto second_before = f.Sync(*f.second);
    const auto topbar_before = f.Sync(*f.topbar);
    const auto dock_before = f.Sync(*f.dock);
    f.Reveal();
    assert(f.Sync(*f.first).buttons == first_before.buttons);
    assert(f.Sync(*f.second).buttons == second_before.buttons);
    assert(f.Sync(*f.topbar).buttons == topbar_before.buttons);
    assert(f.Sync(*f.dock).buttons == dock_before.buttons);
    f.Transition(contracts::LayoutControlIntent::ExitImmersive);
    f.Mode(contracts::LayoutGroupMode::Normal);

    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.FocusFirst();
    const auto first_keys = f.Sync(*f.first).keys;
    const auto second_keys = f.Sync(*f.second).keys;
    f.Key(33, true); // Super+Shift+F: WM-owned desktop-group recovery.
    f.Mode(contracts::LayoutGroupMode::Normal);
    assert(f.Sync(*f.first).keys == first_keys);
    assert(f.Sync(*f.second).keys == second_keys);

    // Revealing does not create a persistent transparent input overlay.
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.Reveal();
    const auto visible_buttons = f.Sync(*f.topbar).buttons;
    f.Move(100, 180);
    f.Sync(*f.topbar);
    f.ReplayEnd(); // Reveal and another hide preserve the terminal journal.
    f.Move(f.server.GetLayoutSnapshot()->outputs.front().logical_bounds.width / 2, 20);
    f.Button(true);
    f.Button(false);
    assert(f.Sync(*f.topbar).buttons == visible_buttons);
    f.Reveal();
    f.Transition(contracts::LayoutControlIntent::ExitImmersive);

    // A press that began inside an application remains its sequence even if
    // the pointer crosses the recovery strip before release.
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    const auto bounds = View(*f.server.GetLayoutSnapshot(), f.first_permit.instance).target_bounds;
    const auto first_buttons = f.Sync(*f.first).buttons;
    const auto other_buttons = f.Sync(*f.second).buttons;
    const auto shell_buttons = f.Sync(*f.topbar).buttons;
    f.Move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
    f.Button(true);
    f.Move(f.server.GetLayoutSnapshot()->outputs.front().logical_bounds.width / 2, 1);
    f.Button(false);
    const auto first_after = f.Sync(*f.first).buttons;
    const auto other_after = f.Sync(*f.second).buttons;
    const auto shell_after = f.Sync(*f.topbar).buttons;
    if (first_after != first_buttons + 2 || shell_after != shell_buttons) {
        std::fprintf(stderr,
                     "Application-owned edge crossing: first buttons %u -> %u, "
                     "second %u -> %u, Topbar %u -> %u; seat button_count=%zu\n",
                     first_buttons, first_after, other_buttons, other_after, shell_buttons,
                     shell_after, f.server.GetSeat()->pointer_state.button_count);
    }
    assert(first_after == first_buttons + 2);
    assert(shell_after == shell_buttons);
    f.Mode(contracts::LayoutGroupMode::Immersive);
    f.Key(33, true);
    f.Mode(contracts::LayoutGroupMode::Normal);
}

void TestDelayedFrontendRecovery(Fixture &f)
{
    // Real seat Up and further pointer motion can precede the Host's first
    // control message. The released proof must retain its existing grace.
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.Reveal();
    auto proof = Press(f.server, *f.topbar, &f.pointer.base);
    Release(f.server, &f.pointer.base);
    f.Move(100, 180);
    f.Sync(*f.topbar); // Roundtrip forces the WM loop to observe the outside pointer.
    f.Move(120, 200);
    f.Sync(*f.topbar);
    f.CompleteGesture(proof, contracts::LayoutControlIntent::ExitImmersive);
    f.Mode(contracts::LayoutGroupMode::Normal);

    // A drag with no final layout intent ends the temporary protection. Once
    // the End(None) reply has arrived, the outside pointer allows hiding again.
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.Reveal();
    proof = Press(f.server, *f.topbar, &f.pointer.base);
    Release(f.server, &f.pointer.base);
    f.Move(100, 180);
    f.Sync(*f.topbar);
    f.CompleteGesture(proof, contracts::LayoutControlIntent::None);
    f.Mode(contracts::LayoutGroupMode::Immersive);
    const auto buttons = f.Sync(*f.topbar).buttons;
    f.Move(f.server.GetLayoutSnapshot()->outputs.front().logical_bounds.width / 2, 20);
    f.Button(true);
    f.Button(false);
    assert(f.Sync(*f.topbar).buttons == buttons);

    f.Reveal();
    f.Transition(contracts::LayoutControlIntent::ExitImmersive);
    f.Mode(contracts::LayoutGroupMode::Normal);
}

void FindHeadless(wlr_backend *backend, void *data)
{
    if (wlr_backend_is_headless(backend)) {
        *static_cast<wlr_backend **>(data) = backend;
    } else if (wlr_backend_is_multi(backend)) {
        wlr_multi_for_each_backend(backend, FindHeadless, data);
    }
}

void TestWorkspaceAndOutput(Fixture &f)
{
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.Reveal();
    f.Key(3); // Super+2.
    f.Mode(contracts::LayoutGroupMode::Normal);
    f.Key(2); // Super+1 keeps the workspace's own group mode.
    f.Mode(contracts::LayoutGroupMode::Immersive);

    // A stale reveal must not make a hidden Topbar collect new input after
    // workspace changes. Return to the central edge to reveal it afresh.
    auto buttons = f.Sync(*f.topbar).buttons;
    const auto width = f.server.GetLayoutSnapshot()->outputs.front().logical_bounds.width;
    f.Move(width / 2, 20);
    f.Button(true);
    f.Button(false);
    assert(f.Sync(*f.topbar).buttons == buttons);
    f.Reveal();
    f.Transition(contracts::LayoutControlIntent::ExitImmersive);

    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.Reveal();
    auto *output = wlr_output_layout_get_center_output(f.server.GetOutputLayout());
    assert(output);
    wlr_output_state state{};
    wlr_output_state_init(&state);
    wlr_output_state_set_scale(&state, 2.0f);
    assert(wlr_output_commit_state(output, &state));
    wlr_output_state_finish(&state);
    f.Settle();
    buttons = f.Sync(*f.topbar).buttons;
    const auto scaled_width = f.server.GetLayoutSnapshot()->outputs.front().logical_bounds.width;
    f.Move(scaled_width / 2, 20);
    f.Button(true);
    f.Button(false);
    assert(f.Sync(*f.topbar).buttons == buttons);
    f.Reveal();
    f.Transition(contracts::LayoutControlIntent::ExitImmersive);

    // Recreated output identities never inherit a stale mode or reveal.
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.Reveal();
    const auto old_id = f.server.GetLayoutSnapshot()->outputs.front().id;
    wlr_backend *headless{};
    FindHeadless(f.server.GetBackend(), &headless);
    assert(headless);
    wlr_output_destroy(output);
    Until(f.server, [&] { return f.server.GetLayoutSnapshot()->outputs.empty(); });
    assert(wlr_headless_add_output(headless, 1024, 768));
    Until(f.server, [&] { return f.server.GetLayoutSnapshot()->outputs.size() == 1; });
    f.Settle();
    assert(f.server.GetLayoutSnapshot()->outputs.front().id != old_id);
    f.Mode(contracts::LayoutGroupMode::Normal);
}

void TestShellFailure(Fixture &f)
{
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.FocusFirst();
    f.Key(33);
    f.topbar->Stop();
    Until(f.server, [&] { return f.topbar->Reaped(); });
    f.Settle();
    f.Mode(contracts::LayoutGroupMode::Normal);
    assert(!View(*f.server.GetLayoutSnapshot(), f.first_permit.instance).fullscreen);
    assert(View(*f.server.GetLayoutSnapshot(), f.second_permit.instance).visible);
    assert(f.server.ControlHealthy());

    // Launcher revocation removes a failed endpoint and permits replacement.
    launch::ControlMessage revoke;
    revoke.type = launch::ControlType::Revoke;
    revoke.permit = f.topbar_permit;
    f.peer.Send(revoke);
    f.server.RunEventLoopIteration(5);
    f.StartTopbar();
    f.Transition(contracts::LayoutControlIntent::EnterImmersive);
    f.FocusFirst();
    f.Key(33);
    revoke.permit = f.topbar_permit;
    f.peer.Send(revoke);
    Until(f.server, [&] { return f.topbar->Reaped(); });
    f.Settle();
    f.Mode(contracts::LayoutGroupMode::Normal);
    assert(!View(*f.server.GetLayoutSnapshot(), f.first_permit.instance).fullscreen);
    assert(View(*f.server.GetLayoutSnapshot(), f.second_permit.instance).visible);
    assert(f.server.ControlHealthy());
}

void TestGroup(wm::WlrServer &server, ControlPeer &peer)
{
    Fixture fixture(server, peer);
    TestModeAndOverlay(fixture);
    TestThemeAndConfigure(fixture);
    TestMouseAndKeyboardRecovery(fixture);
    TestDelayedFrontendRecovery(fixture);
    TestWorkspaceAndOutput(fixture);
    TestShellFailure(fixture);
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
    assert(server.Initialize("wayland-control-" + std::to_string(getpid())));
    server.AttachControl(server_fd, getppid());
    server.Start();
    assert(server.InstallTheme(test::WmThemeFixture()).success);

    ControlPeer peer(peer_fd);
    peer.Collect();
    assert(peer.messages.size() == 1 && peer.messages.front().type == launch::ControlType::Ready);
    peer.session = peer.messages.front().permit.session;
    TestGroup(server, peer);
    server.Stop();
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--client") {
        return RunClient(argv[2], std::stoi(argv[3]));
    }
    assert(argc == 1);
    char directory[] = "/tmp/prism-native-group.XXXXXX";
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
    std::puts("Native group immersion: typed authority, tree preservation, overlay modes, "
              "mouse recovery, keyboard recovery and Shell/output cleanup passed");
}
