#include "prism/contracts/theme.hpp"
#include "prism/runtime/prepared_component.hpp"
#include "prism/sdk/client_application.hpp"
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {
using Application = prism::sdk::ClientApplication;
using Stats = prism::sdk::ClientRenderStats;
using prism::runtime::ComponentSource;
using prism::runtime::LoadDiagnostic;
using prism::runtime::LoadFailure;
using prism::runtime::LoadStage;
using prism::runtime::PreparedComponent;
using prism::runtime::UiLoadId;
using namespace std::chrono_literals;

constexpr std::string_view initial_source = R"(
    Card(background:#182840FF,padding:14) {
        VStack(spacing:12) {
            Text($original,font:20,foreground:"@foreground")
            Image("checker.png",width:72,height:72)
        }
    }
)";

void Require(bool condition, std::string_view detail)
{
    if (!condition) {
        throw std::runtime_error(std::string(detail));
    }
}

struct PrepareTask {
    std::string source;
    ComponentSource metadata;
    std::promise<PreparedComponent> result;

    void operator()()
    {
        try {
            result.set_value(prism::runtime::PrepareComponent(source, std::move(metadata)));
        } catch (...) {
            result.set_exception(std::current_exception());
        }
    }
};

PreparedComponent PrepareOnWorker(std::string_view source, std::string id)
{
    std::promise<PreparedComponent> result;
    auto future = result.get_future();
    std::thread worker(PrepareTask{
        std::string(source), {std::move(id), "prepared_ui_probe.prism", "v1"}, std::move(result)});
    worker.join();
    return future.get();
}

prism::sdk::ClientConfig Config(const std::string &socket, const std::string &assets,
                                std::string id)
{
    prism::sdk::ClientConfig config;
    config.socket = socket;
    config.app_id = std::move(id);
    config.title = "Prepared UI lifecycle probe";
    config.font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    config.assets_root = assets;
    config.width = 480;
    config.height = 320;
    return config;
}

prism::contracts::ThemeSnapshot Theme()
{
    prism::contracts::ThemeSnapshot theme;
    theme.id = "prepared-probe";
    theme.name = "Prepared UI probe";
    theme.generation = 1;
    theme.colors = {{"foreground", {235, 240, 250, 255}}};
    return theme;
}

void WaitFor(Application &app, int presentations, int images)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (app.PresentationCount() < presentations || app.LoadedImageCount() < images ||
           app.FrameCallbackPending()) {
        Require(std::chrono::steady_clock::now() < deadline,
                "Timed out waiting for presented prepared UI/resources");
        Require(app.Pump(20), "Prepared client stopped while awaiting presentation");
    }
    for (int i = 0; i < 4; ++i) {
        Require(app.Pump(0), "Prepared client stopped while draining events");
    }
    // A decoded image can be applied after a Pump returns and submitted by the
    // next Pump. Finish that callback too before inspecting rejection counters.
    while (app.FrameCallbackPending()) {
        Require(std::chrono::steady_clock::now() < deadline, "Final pixel callback did not finish");
        Require(app.Pump(20), "Prepared client stopped during final pixel callback");
    }
}

void NoWork(const Stats &before, const Stats &after, std::string_view detail)
{
    Require(before.scene_build_attempts == after.scene_build_attempts &&
                before.scene_builds == after.scene_builds &&
                before.scene_layouts == after.scene_layouts &&
                before.gpu_render_attempts == after.gpu_render_attempts &&
                before.swap_attempts == after.swap_attempts &&
                before.surface_pixel_commits == after.surface_pixel_commits,
            detail);
}

void Print(std::string_view scenario, const Application &app)
{
    const auto stats = app.GetRenderStats();
    std::cout << "scenario=" << scenario << " images_requested=" << app.RequestedImageCount()
              << " images_loaded=" << app.LoadedImageCount()
              << " presentation_count=" << app.PresentationCount()
              << " gpu_draws=" << stats.gpu_render_successes << " swaps=" << stats.swap_successes
              << " gl_renderer=" << app.GlRenderer() << '\n';
}

void RejectToken(Application &app, UiLoadId token, const PreparedComponent &prepared, bool open,
                 bool lifecycle_failure = false)
{
    const auto stats = app.GetRenderStats();
    const auto requested = app.RequestedImageCount();
    const auto loaded = app.LoadedImageCount();
    LoadDiagnostic diagnostic;
    const bool accepted = open ? app.OpenPrepared(token, prepared, &diagnostic)
                               : app.ReplaceUiPrepared(token, prepared, &diagnostic);
    Require(!accepted, "Stale/cancelled/foreign/consumed token unexpectedly installed");
    Require((diagnostic.stage == LoadStage::Cancelled ||
             (lifecycle_failure && diagnostic.stage == LoadStage::Install)) &&
                !diagnostic.message.empty(),
            "Rejected UI load lacks lifecycle/cancellation diagnostic");
    Require(app.RequestedImageCount() == requested && app.LoadedImageCount() == loaded,
            "Rejected token touched the resource table");
    if (app.IsMapped()) {
        Require(app.Pump(0), "Client stopped after rejecting UI load token");
    }
    NoWork(stats, app.GetRenderStats(), "Rejected token built/rendered/swapped UI");
}

void CheckBadPreparation(Application &app)
{
    const auto stats = app.GetRenderStats();
    const auto requested = app.RequestedImageCount();
    bool rejected = false;
    try {
        PrepareOnWorker(R"(
            VStack {
                Image("checker.png",width:40,height:40)
                Text("bad",nonexistentProperty:1)
            }
        )",
                        "invalid-schema");
    } catch (const LoadFailure &error) {
        rejected = true;
        Require(error.Diagnostic().stage == LoadStage::Semantic,
                "Invalid property reported outside semantic preparation");
        Require(error.Diagnostic().source.component_id == "invalid-schema" &&
                    !error.Diagnostic().message.empty(),
                "Bad preparation lost source identity/diagnostic");
    }
    Require(rejected, "Schema error unexpectedly prepared");
    Require(app.RequestedImageCount() == requested, "Bad preparation requested client images");
    NoWork(stats, app.GetRenderStats(), "Bad preparation touched submitted UI");
}

void CheckThemeRetry(Application &app, prism::contracts::ThemeSnapshot &theme)
{
    const auto token = app.BeginUiLoad();
    const auto prepared =
        PrepareOnWorker(R"(Text($replacement,font:22,foreground:"@late_token"))", "late-theme");
    const auto stats = app.GetRenderStats();
    const auto requested = app.RequestedImageCount();
    LoadDiagnostic diagnostic;
    Require(!app.ReplaceUiPrepared(token, prepared, &diagnostic),
            "Missing theme reference unexpectedly installed");
    Require(diagnostic.stage == LoadStage::Install && !diagnostic.message.empty(),
            "Missing theme token lacks install diagnostic");
    Require(app.RequestedImageCount() == requested, "Text-only failed candidate requested images");
    Require(app.SetBinding("original", std::string("preserved")),
            "Failed candidate replaced the old Scene/bindings");
    Require(app.Pump(0), "Old Scene stopped after failed candidate");
    NoWork(stats, app.GetRenderStats(), "Failed candidate changed old binding/rendered pixels");

    // The prepared result predates this snapshot. Install must use the current
    // owner snapshot and a failed install must leave its same token retryable.
    theme.colors.push_back({"late_token", {125, 195, 245, 255}});
    ++theme.generation;
    Require(app.ApplyTheme(theme), "Current snapshot with late token was rejected");
    Require(app.ThemeGeneration() == theme.generation, "Latest snapshot was not retained");
    const auto presentations = app.PresentationCount();
    Require(app.ReplaceUiPrepared(token, prepared, &diagnostic),
            "Same-generation retry with latest theme failed");
    Require(app.SetBinding("replacement", std::string("Latest theme installed")),
            "Retried prepared Scene missing its binding");
    Require(!app.SetBinding("original", std::string("obsolete")),
            "Successful replacement retained the obsolete Scene");
    WaitFor(app, presentations + 1, 1);
    RejectToken(app, token, prepared, false, true);
    Print("failed-theme-retry-latest-snapshot", app);
}

void CheckResourceFailure(Application &app)
{
    const auto token = app.BeginUiLoad();
    const auto prepared =
        PrepareOnWorker(R"(Image("../checker.png",width:40,height:40))", "invalid-resource-path");
    const auto stats = app.GetRenderStats();
    LoadDiagnostic diagnostic;
    Require(!app.ReplaceUiPrepared(token, prepared, &diagnostic),
            "Out-of-root image URI unexpectedly installed");
    Require(diagnostic.stage == LoadStage::ResourceLink && !diagnostic.message.empty(),
            "Image path failure lacks resource-link diagnostic");
    Require(app.SetBinding("replacement", std::string("Latest theme installed")),
            "Resource path failure removed retained Scene/binding");
    Require(app.Pump(0), "Retained Scene stopped after resource-link failure");
    NoWork(stats, app.GetRenderStats(), "Resource-link failure rendered new UI");
    app.CancelUiLoad();
    Print("resource-link-failure-retains-scene", app);
}

void CheckOpenRetry(const std::string &socket, const std::string &assets)
{
    const auto valid = Config(socket, assets, "prepared_open_retry");
    auto unavailable = valid;
    unavailable.socket += ".missing";
    Application app(std::move(unavailable));
    const auto token = app.BeginUiLoad();
    const auto prepared =
        PrepareOnWorker(R"(Text($original,font:20,foreground:#FFFFFFFF))", "open-retry");
    LoadDiagnostic diagnostic;
    Require(!app.OpenPrepared(token, prepared, &diagnostic),
            "Missing Wayland socket unexpectedly opened");
    Require(diagnostic.stage == LoadStage::Install && !diagnostic.message.empty(),
            "Connection failure lacks install diagnostic");
    Require(app.RequestedImageCount() == 0 && app.GetRenderStats().swap_attempts == 0,
            "Failed text-only connection requested resources/submitted pixels");

    Require(app.ConfigureWindow(valid), "Failed open prevented corrected window configuration");
    Require(app.OpenPrepared(token, prepared, &diagnostic),
            "Same token could not retry after corrected Wayland socket");
    Require(app.SetBinding("original", std::string("Connection retry succeeded")),
            "Retried open did not install its Scene/binding");
    Require(app.HasPresentationFeedback(), "Retried connection lacks presentation-time");
    WaitFor(app, 1, 0);
    Require(app.GlRenderer().find("V3D") != std::string::npos, "Retried open did not use V3D");
    Print("connection-failure-same-token-retry", app);
    app.Close();
}

int Verify(const std::string &socket, const std::string &assets)
{
    CheckOpenRetry(socket, assets);

    Application first(Config(socket, assets, "prepared_first"));
    Application second(Config(socket, assets, "prepared_second"));
    auto theme = Theme();
    Require(first.ApplyTheme(theme) && second.ApplyTheme(theme), "Probe theme was rejected");
    Require(first.FrontendReady() && second.FrontendReady(), "Probe fonts were not ready");

    const auto first_token = first.BeginUiLoad();
    const auto second_token = second.BeginUiLoad();
    const auto prepared = PrepareOnWorker(initial_source, "shared-image-ui");
    Require(prepared.Images().size() == 1 && prepared.Images().front().uri == "checker.png",
            "Prepared image table did not retain its symbolic URI");
    Require(first.RequestedImageCount() == 0 && second.RequestedImageCount() == 0,
            "Preparation prematurely allocated owner resources");

    RejectToken(second, first_token, prepared, true);
    RejectToken(first, second_token, prepared, true);
    Require(first.OpenPrepared(first_token, prepared), "Initial prepared open failed");
    Require(first.SetBinding("original", std::string("preserved")), "Initial binding was missing");
    Require(first.HasPresentationFeedback(), "Compositor lacks real presentation feedback");
    WaitFor(first, 1, 1);
    Require(first.GlRenderer().find("V3D") != std::string::npos, "First client did not use V3D");
    Require(first.RequestedImageCount() == 1, "Initial image was not linked exactly once");
    Print("initial-worker-prepared-open", first);

    RejectToken(first, first_token, prepared, false, true);
    const auto stale = first.BeginUiLoad();
    const auto cancelled = first.BeginUiLoad();
    RejectToken(first, stale, prepared, false);
    RejectToken(first, stale, prepared, false);
    first.CancelUiLoad();
    RejectToken(first, cancelled, prepared, false);
    RejectToken(first, second_token, prepared, false);
    CheckBadPreparation(first);
    CheckThemeRetry(first, theme);
    CheckResourceFailure(first);

    // One immutable result links into two independent owner resource tables;
    // numerical IDs may coincide because each table has its own namespace.
    Require(second.OpenPrepared(second_token, prepared), "Shared result failed for second owner");
    Require(second.SetBinding("original", std::string("independent")),
            "Second binding was missing");
    WaitFor(second, 1, 1);
    Require(second.GlRenderer().find("V3D") != std::string::npos, "Second client did not use V3D");
    Require(second.RequestedImageCount() == 1 && second.LoadedImageCount() == 1,
            "Second owner did not link/decode its own image");
    Print("shared-result-independent-resource-owner", second);

    const auto closed_token = first.BeginUiLoad();
    first.Close();
    RejectToken(first, closed_token, prepared, false, true);
    const auto presentations = second.PresentationCount();
    Require(second.SetBinding("original", std::string("First owner closed; image remains live")),
            "Closing first owner broke second binding");
    WaitFor(second, presentations + 1, 1);
    Print("independent-owner-survives-close", second);

    const auto second_closed_token = second.BeginUiLoad();
    second.Close();
    RejectToken(second, second_closed_token, prepared, true, true);
    std::cout << "prepared_ui_probe: passed\n";
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "Usage: prepared_ui_probe WAYLAND_SOCKET ASSETS_ROOT\n";
        return 2;
    }
    try {
        return Verify(argv[1], argv[2]);
    } catch (const std::exception &error) {
        std::cerr << "prepared_ui_probe: " << error.what() << '\n';
        return 1;
    }
}
