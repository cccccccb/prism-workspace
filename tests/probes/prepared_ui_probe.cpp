#include "prism/contracts/theme.hpp"
#include "prism/runtime/prepared_component.hpp"
#include "prism/sdk/client_application.hpp"
#include <chrono>
#include <exception>
#include <functional>
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
    Card(background:#182840FF,padding:14,backdropBlur:$blur) {
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

void WaitForUi(Application &app, UiLoadId load, int images)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!app.GetUiPresentation(load).presented || app.LoadedImageCount() < images ||
           app.FrameCallbackPending()) {
        Require(std::chrono::steady_clock::now() < deadline,
                "Timed out waiting for this UI generation's presentation/resources");
        Require(app.Pump(20), "Prepared client stopped while awaiting UI presentation");
    }
    const auto state = app.GetUiPresentation(load);
    Require(state.installed && state.submitted && state.presented && state.first_submission > 0 &&
                state.last_presented_submission >= state.first_submission,
            "UI presented without a matching successful pixel submission");
    WaitFor(app, app.PresentationCount(), images);
}

struct UiSubmitObserver {
    Application *application;
    std::thread::id owner{std::this_thread::get_id()};
    UiLoadId last{};
    unsigned callbacks{};

    void Submitted(UiLoadId load)
    {
        Require(std::this_thread::get_id() == owner, "UI observer ran off the owner thread");
        const auto state = application->GetUiPresentation(load);
        Require(state.installed && state.submitted && state.last_submission > 0,
                "UI submit observer ran before identity mapping was installed");
        last = load;
        ++callbacks;
    }
};

void NoWork(const Stats &before, const Stats &after, std::string_view detail);

void CheckStateOnlyIdentity(Application &app, UiLoadId load, UiSubmitObserver &observer)
{
    const auto before = app.GetUiPresentation(load);
    const auto stats = app.GetRenderStats();
    const auto callbacks = observer.callbacks;
    Require(app.SetBinding("blur", 8.0), "State-only blur binding was missing");
    Require(app.Pump(0), "State-only metadata submission stopped client");
    const auto after = app.GetUiPresentation(load);
    Require(after.first_submission == before.first_submission &&
                after.last_submission == before.last_submission &&
                after.submitted_count == before.submitted_count && observer.callbacks == callbacks,
            "State-only commit acquired a pixel/UI submission identity");
    Require(app.GetRenderStats().surface_state_commits == stats.surface_state_commits + 1,
            "Blur metadata did not commit state independently");
    NoWork(stats, app.GetRenderStats(), "State-only metadata rendered or swapped UI");
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

void Print(std::string_view scenario, const Application &app, UiLoadId load = {})
{
    const auto stats = app.GetRenderStats();
    const auto ui = app.GetUiPresentation(load);
    std::cout << "scenario=" << scenario << " images_requested=" << app.RequestedImageCount()
              << " images_loaded=" << app.LoadedImageCount()
              << " presentation_count=" << app.PresentationCount()
              << " gpu_draws=" << stats.gpu_render_successes << " swaps=" << stats.swap_successes
              << " ui_generation=" << load.generation << " ui_submitted=" << ui.submitted
              << " ui_presented=" << ui.presented << " first_submission=" << ui.first_submission
              << " presented_submission=" << ui.last_presented_submission
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

void CheckThemeRetry(Application &app, prism::contracts::ThemeSnapshot &theme, UiLoadId prior)
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
    const auto installed = app.GetUiPresentation(token);
    Require(app.PresentationCount() > 0 && installed.installed && !installed.submitted &&
                !installed.presented && installed.last_presented_submission == 0,
            "Old Preview feedback/count proved a newly installed Master");
    Require(app.SetBinding("replacement", std::string("Latest theme installed")),
            "Retried prepared Scene missing its binding");
    Require(!app.SetBinding("original", std::string("obsolete")),
            "Successful replacement retained the obsolete Scene");
    WaitForUi(app, token, 1);
    Require(app.PresentationCount() >= presentations + 1 &&
                app.GetUiPresentation(token).first_submission >
                    app.GetUiPresentation(prior).last_submission,
            "Replacement UI did not use a newer successful pixel submission");
    RejectToken(app, token, prepared, false, true);
    Print("failed-theme-retry-latest-snapshot", app, token);
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
    Require(!app.GetUiPresentation(token).installed,
            "Failed window open retained an installed UI presentation record");

    Require(app.ConfigureWindow(valid), "Failed open prevented corrected window configuration");
    Require(app.OpenPrepared(token, prepared, &diagnostic),
            "Same token could not retry after corrected Wayland socket");
    Require(app.SetBinding("original", std::string("Connection retry succeeded")),
            "Retried open did not install its Scene/binding");
    Require(app.HasPresentationFeedback(), "Retried connection lacks presentation-time");
    WaitForUi(app, token, 0);
    Require(app.GlRenderer().find("V3D") != std::string::npos, "Retried open did not use V3D");
    Print("connection-failure-same-token-retry", app, token);
    app.Close();
}

int Verify(const std::string &socket, const std::string &assets)
{
    CheckOpenRetry(socket, assets);

    Application first(Config(socket, assets, "prepared_first"));
    Application second(Config(socket, assets, "prepared_second"));
    auto theme = Theme();
    UiSubmitObserver observer{&first};
    first.OnUiSubmitted(std::bind_front(&UiSubmitObserver::Submitted, &observer));
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
    Require(first.GetUiPresentation(first_token).installed &&
                !first.GetUiPresentation(first_token).submitted &&
                !first.GetUiPresentation(first_token).presented,
            "Installing initial UI was mistaken for a pixel submission/presentation");
    Require(first.SetBinding("original", std::string("preserved")), "Initial binding was missing");
    Require(first.HasPresentationFeedback(), "Compositor lacks real presentation feedback");
    WaitForUi(first, first_token, 1);
    Require(observer.callbacks > 0 && observer.last == first_token,
            "Successful initial UI did not notify its submission observer");
    Require(first.GlRenderer().find("V3D") != std::string::npos, "First client did not use V3D");
    Require(first.RequestedImageCount() == 1, "Initial image was not linked exactly once");
    Print("initial-worker-prepared-open", first, first_token);
    CheckStateOnlyIdentity(first, first_token, observer);
    Print("state-only-no-ui-submission", first, first_token);

    RejectToken(first, first_token, prepared, false, true);
    const auto stale = first.BeginUiLoad();
    const auto cancelled = first.BeginUiLoad();
    RejectToken(first, stale, prepared, false);
    RejectToken(first, stale, prepared, false);
    first.CancelUiLoad();
    RejectToken(first, cancelled, prepared, false);
    RejectToken(first, second_token, prepared, false);
    CheckBadPreparation(first);
    CheckThemeRetry(first, theme, first_token);
    CheckResourceFailure(first);

    // One immutable result links into two independent owner resource tables;
    // numerical IDs may coincide because each table has its own namespace.
    Require(second.OpenPrepared(second_token, prepared), "Shared result failed for second owner");
    Require(second.SetBinding("original", std::string("independent")),
            "Second binding was missing");
    WaitForUi(second, second_token, 1);
    Require(second.GlRenderer().find("V3D") != std::string::npos, "Second client did not use V3D");
    Require(second.RequestedImageCount() == 1 && second.LoadedImageCount() == 1,
            "Second owner did not link/decode its own image");
    Print("shared-result-independent-resource-owner", second, second_token);

    const auto closed_token = first.BeginUiLoad();
    first.Close();
    Require(!first.GetUiPresentation(first_token).installed,
            "Closed frontend retained UI submission/presentation mapping");
    RejectToken(first, closed_token, prepared, false, true);
    const auto presentations = second.PresentationCount();
    Require(second.SetBinding("original", std::string("First owner closed; image remains live")),
            "Closing first owner broke second binding");
    WaitFor(second, presentations + 1, 1);
    Print("independent-owner-survives-close", second);

    const auto second_closed_token = second.BeginUiLoad();
    second.Close();
    Require(!second.GetUiPresentation(second_token).installed,
            "Closed second frontend retained UI identity mapping");
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
