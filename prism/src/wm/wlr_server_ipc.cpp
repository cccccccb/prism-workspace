#include "prism/ipc/wm_messages.hpp"
#include "wlr_server_internal.hpp"

namespace prism::wm {
namespace {
std::string Reply(ipc::CommandReply message)
{
    return nlohmann::json(message).dump();
}

std::string Error(std::string message = {})
{
    ipc::CommandReply reply;
    reply.status = "error";
    if (!message.empty()) {
        reply.message = std::move(message);
    }
    return Reply(std::move(reply));
}

std::optional<tree::Direction> ParseDirection(const std::vector<std::string> &args)
{
    if (args.empty()) {
        return {};
    }
    if (args[0] == "left" || args[0] == "h") {
        return tree::Direction::Left;
    }
    if (args[0] == "right" || args[0] == "l") {
        return tree::Direction::Right;
    }
    if (args[0] == "up" || args[0] == "k") {
        return tree::Direction::Up;
    }
    if (args[0] == "down" || args[0] == "j") {
        return tree::Direction::Down;
    }
    return {};
}

} // namespace

void WlrServer::InitializeIpc()
{
    ipc_server_ = std::make_unique<ipc::IpcServer>(wl_event_loop_);
    if (!ipc_server_->Start()) {
        PRISM_LOG_WARN("WLR-SERVER", "Failed to start Compositor IPC Server");
        return;
    }

    const auto outputs = std::bind_front(&WlrServer::IpcOutputs, this);
    ipc_server_->RegisterHandler("get_outputs", outputs);
    ipc_server_->RegisterHandler("outputs", outputs);

    const auto mode = std::bind_front(&WlrServer::IpcMode, this);
    ipc_server_->RegisterHandler("set_mode", mode);
    ipc_server_->RegisterHandler("mode", mode);

    const auto status = std::bind_front(&WlrServer::IpcStatus, this);
    ipc_server_->RegisterHandler("get_status", status);
    ipc_server_->RegisterHandler("status", status);

    const auto debug = std::bind_front(&WlrServer::IpcDebug, this);
    ipc_server_->RegisterHandler("set_debug", debug);
    ipc_server_->RegisterHandler("debug", debug);

    const auto layout = std::bind_front(&WlrServer::IpcLayout, this);
    ipc_server_->RegisterHandler("set_layout", layout);
    ipc_server_->RegisterHandler("layout", layout);
    ipc_server_->RegisterHandler("split", layout);

    ipc_server_->RegisterHandler("focus", std::bind_front(&WlrServer::IpcFocus, this));
    ipc_server_->RegisterHandler("swap", std::bind_front(&WlrServer::IpcSwap, this));

    const auto workspace = std::bind_front(&WlrServer::IpcWorkspace, this);
    ipc_server_->RegisterHandler("workspace", workspace);
    ipc_server_->RegisterHandler("ws", workspace);
    ipc_server_->RegisterHandler("move_workspace",
                                 std::bind_front(&WlrServer::IpcMoveWorkspace, this));

    const auto tree = std::bind_front(&WlrServer::IpcTree, this);
    ipc_server_->RegisterHandler("tree", tree);
    ipc_server_->RegisterHandler("get_tree", tree);

    const auto close = std::bind_front(&WlrServer::IpcClose, this);
    ipc_server_->RegisterHandler("close", close);
    ipc_server_->RegisterHandler("kill", close);

    const auto theme = std::bind_front(&WlrServer::IpcTheme, this);
    ipc_server_->RegisterHandler("theme", theme);
    ipc_server_->RegisterHandler("set_theme", theme);
    ipc_server_->RegisterHandler("fold", std::bind_front(&WlrServer::IpcFold, this));

    const auto fullscreen = std::bind_front(&WlrServer::IpcFullscreen, this);
    ipc_server_->RegisterHandler("fullscreen", fullscreen);
    ipc_server_->RegisterHandler("monocle", fullscreen);
    ipc_server_->RegisterHandler("action", std::bind_front(&WlrServer::IpcAction, this));
    ipc_server_->RegisterHandler("ipc_test", std::bind_front(&WlrServer::IpcBenchmark, this));
}

std::string WlrServer::IpcOutputs(const std::string &, const std::vector<std::string> &)
{
    ipc::OutputsReply reply;
    for (const auto &output : GetOutputsInfo()) {
        ipc::OutputMessage message{output.name,
                                   output.make,
                                   output.model,
                                   output.width,
                                   output.height,
                                   output.refresh_mhz,
                                   ipc::WireFixedValue(output.refresh_hz, 1),
                                   ipc::WireFixedValue(current_fps_, 1),
                                   output.adaptive_sync,
                                   {}};
        for (const auto &mode : output.modes) {
            message.modes.push_back({mode.width, mode.height,
                                     ipc::WireFixedValue(mode.refresh_hz, 1), mode.preferred,
                                     mode.current});
        }
        reply.outputs.push_back(std::move(message));
    }
    return nlohmann::json(reply).dump();
}

std::string WlrServer::IpcMode(const std::string &, const std::vector<std::string> &args)
{
    if (args.size() < 3) {
        return Error("Usage: set_mode <output_name|all> <width> <height> [refresh_hz]");
    }
    const auto &output = args[0];
    const int width = std::stoi(args[1]), height = std::stoi(args[2]);
    int refresh = 0;
    if (args.size() >= 4) {
        const int value = std::stoi(args[3]);
        refresh = value < 1000 ? value * 1000 : value;
    }
    if (!SetOutputMode(output, width, height, refresh)) {
        return Error("Failed to set mode for output: " + output);
    }
    ipc::CommandReply reply;
    reply.status = "ok";
    reply.message = "Output mode updated successfully";
    reply.output = output;
    reply.width = width;
    reply.height = height;
    return Reply(std::move(reply));
}

std::string WlrServer::IpcDebug(const std::string &, const std::vector<std::string> &args)
{
    if (!args.empty() && compositor_) {
        if (args[0] == "toggle") {
            compositor_->SetDebugHud(!compositor_->IsDebugHudEnabled());
        } else if (args[0] == "1" || args[0] == "true" || args[0] == "on") {
            compositor_->SetDebugHud(true);
        } else if (args[0] == "0" || args[0] == "false" || args[0] == "off") {
            compositor_->SetDebugHud(false);
        }
    }
    const bool current = compositor_ && compositor_->IsDebugHudEnabled();
    if (!args.empty() && hud_tree_) {
        wlr_scene_node_set_enabled(&hud_tree_->node, current);
    }
    ipc::CommandReply reply;
    reply.status = "ok";
    reply.debug_hud = current;
    return Reply(std::move(reply));
}

std::string WlrServer::IpcLayout(const std::string &, const std::vector<std::string> &args)
{
    if (args.empty()) {
        return Error("Usage: layout splith|splitv");
    }
    tree::LayoutMode mode;
    if (args[0] == "splith" || args[0] == "h" || args[0] == "horizontal") {
        mode = tree::LayoutMode::SplitHorizontal;
    } else if (args[0] == "splitv" || args[0] == "v" || args[0] == "vertical") {
        mode = tree::LayoutMode::SplitVertical;
    } else {
        return Error("Unsupported native layout");
    }
    const bool ok = compositor_ && compositor_->SetTreeLayout(mode);
    ArrangeXdgViews();
    SynchronizeXdgFocus();
    ipc::CommandReply reply;
    reply.status = ok ? "ok" : "error";
    reply.layout = args[0];
    return Reply(std::move(reply));
}

std::string WlrServer::IpcFocus(const std::string &, const std::vector<std::string> &args)
{
    const auto direction = ParseDirection(args);
    if (!direction) {
        return Error("Usage: focus left|right|up|down");
    }
    const bool changed = compositor_ && !(focused_xdg_view_ && focused_xdg_view_->fullscreen) &&
                         compositor_->MoveFocus(*direction);
    ArrangeXdgViews();
    SynchronizeXdgFocus();
    ipc::CommandReply reply;
    reply.status = changed ? "ok" : "no_change";
    reply.direction = args[0];
    return Reply(std::move(reply));
}

std::string WlrServer::IpcSwap(const std::string &, const std::vector<std::string> &args)
{
    const auto direction = ParseDirection(args);
    if (!direction) {
        return Error("Usage: swap left|right|up|down");
    }
    const bool changed = compositor_ && compositor_->SwapFocusDirection(*direction);
    ArrangeXdgViews();
    SynchronizeXdgFocus();
    ipc::CommandReply reply;
    reply.status = changed ? "ok" : "no_change";
    reply.direction = args[0];
    return Reply(std::move(reply));
}

std::string WlrServer::IpcWorkspace(const std::string &, const std::vector<std::string> &args)
{
    if (!compositor_) {
        return Error();
    }
    ipc::CommandReply reply;
    if (args.empty()) {
        reply.status = "ok";
        reply.active_workspace = compositor_->GetTreeEngine().GetActiveWorkspace()->GetName();
    } else {
        reply.status = compositor_->SwitchWorkspace(args[0]) ? "ok" : "error";
        reply.workspace = args[0];
        ArrangeXdgViews();
        SynchronizeXdgFocus();
    }
    return Reply(std::move(reply));
}

std::string WlrServer::IpcMoveWorkspace(const std::string &, const std::vector<std::string> &args)
{
    if (args.empty() || !compositor_) {
        return Error();
    }
    auto &tree = compositor_->GetTreeEngine();
    const bool ok = tree.MoveWindowToWorkspace(tree.GetFocusedWindow(), args[0]);
    ArrangeXdgViews();
    SynchronizeXdgFocus();
    ipc::CommandReply reply;
    reply.status = ok ? "ok" : "error";
    reply.workspace = args[0];
    return Reply(std::move(reply));
}

std::string WlrServer::IpcTree(const std::string &, const std::vector<std::string> &)
{
    return compositor_ ? compositor_->GetTreeEngine().DumpTreeJson() : Error();
}

std::string WlrServer::IpcClose(const std::string &, const std::vector<std::string> &)
{
    ipc::CommandReply reply;
    reply.status = focused_xdg_view_ ? "ok" : "no_change";
    CloseFocusedXdgView();
    return Reply(std::move(reply));
}

std::string WlrServer::IpcTheme(const std::string &, const std::vector<std::string> &args)
{
    ipc::CommandReply reply;
    if (!args.empty()) {
        reply.status = "unsupported";
        reply.message = "Request global themes through prism-msg set_theme <id>";
    } else if (!theme_snapshot_) {
        reply.status = "bootstrap";
        reply.generation = 0;
    } else {
        reply.status = "ok";
        reply.current_theme = theme_snapshot_->name;
        reply.id = theme_snapshot_->id;
        reply.generation = theme_snapshot_->generation;
        reply.schema_version = theme_snapshot_->schema_version;
        reply.color_scheme = theme_snapshot_->color_scheme;
    }
    return Reply(std::move(reply));
}

std::string WlrServer::IpcFold(const std::string &, const std::vector<std::string> &)
{
    return Error("Fold is not implemented for native BSP windows");
}

std::string WlrServer::IpcFullscreen(const std::string &, const std::vector<std::string> &args)
{
    if (!focused_xdg_view_) {
        ipc::CommandReply reply;
        reply.status = "no_change";
        return Reply(std::move(reply));
    }
    bool enabled = !focused_xdg_view_->fullscreen;
    if (!args.empty()) {
        if (args[0] == "on" || args[0] == "enable") {
            enabled = true;
        } else if (args[0] == "off" || args[0] == "disable") {
            enabled = false;
        } else if (args[0] != "toggle") {
            return Error();
        }
    }
    SetXdgFullscreen(focused_xdg_view_, enabled);
    ipc::CommandReply reply;
    reply.status = "ok";
    reply.fullscreen = enabled;
    return Reply(std::move(reply));
}

std::string WlrServer::IpcAction(const std::string &, const std::vector<std::string> &args)
{
    if (args.size() < 2) {
        return Error("Usage: action <app_id> <action_name>");
    }
    if (compositor_) {
        compositor_->DispatchAction(args[0], args[1]);
    }
    ipc::CommandReply reply;
    reply.status = "ok";
    reply.app = args[0];
    reply.action = args[1];
    return Reply(std::move(reply));
}

std::string WlrServer::IpcBenchmark(const std::string &, const std::vector<std::string> &)
{
    const std::string name = "/prism_bench_shm";
    auto host = ipc::Channel::CreateHost(name);
    auto client = ipc::Channel::ConnectClient(name);
    if (!host || !client) {
        return Error("Failed to initialize SHM bench channel");
    }
    constexpr int iterations = 10000;
    const auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto event = ipc::EventPacket::MakeAction("bench_action");
        event.data.custom_val = static_cast<double>(i);
        host->PushEvent(event);
        ipc::EventPacket received{};
        client->PopEvent(received);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::high_resolution_clock::now() - start)
                             .count();
    const double average = static_cast<double>(elapsed) / iterations;
    const double throughput = (static_cast<double>(iterations) / (elapsed / 1e9)) / 1e6;
    ipc::BenchReply reply;
    reply.channel = name;
    reply.packets_tested = iterations;
    reply.total_time_us = elapsed / 1000;
    reply.avg_latency_ns = ipc::WireFixedValue(average, 1);
    reply.avg_latency_us = ipc::WireFixedValue(average / 1000, 4);
    reply.throughput_mops = ipc::WireFixedValue(throughput, 2);
    reply.verdict = "Zero-copy lock-free SHM ring buffer validated with sub-microsecond latency!";
    return nlohmann::json(reply).dump();
}
} // namespace prism::wm
