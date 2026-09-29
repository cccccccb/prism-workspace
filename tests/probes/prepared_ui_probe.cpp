#include "prism/contracts/theme.hpp"
#include "prism/runtime/prepared_component.hpp"
#include "prism/sdk/client_application.hpp"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
using prism::runtime::UiInstallState;
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

void WaitForSettledUi(Application &app, UiLoadId load)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::uint64_t previous_swaps = UINT64_MAX;
    int quiet_turns = 0;
    while (quiet_turns < 3) {
        Require(std::chrono::steady_clock::now() < deadline,
                "Prepared UI did not settle before state-only check");
        Require(app.Pump(20), "Prepared UI stopped while settling");

        const auto ui = app.GetUiPresentation(load);
        const auto swaps = app.GetRenderStats().swap_successes;
        const bool settled = app.GetUiInstallStats().image_uploads >=
                                 static_cast<std::uint64_t>(app.LoadedImageCount()) &&
                             ui.last_submission == ui.last_presented_submission &&
                             app.PresentationCount() >= static_cast<int>(ui.presented_count) &&
                             !app.FrameCallbackPending() && swaps == previous_swaps;
        quiet_turns = settled ? quiet_turns + 1 : 0;
        previous_swaps = swaps;
    }
}

void CheckStateOnlyIdentity(Application &app, UiLoadId load, UiSubmitObserver &observer)
{
    WaitForSettledUi(app, load);
    const auto before = app.GetUiPresentation(load);
    const auto stats = app.GetRenderStats();
    const auto callbacks = observer.callbacks;
    Require(app.SetBinding("blur", 8.0), "State-only blur binding was missing");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (app.GetRenderStats().surface_state_commits == stats.surface_state_commits) {
        Require(std::chrono::steady_clock::now() < deadline,
                "Blur metadata did not commit state independently");
        Require(app.Pump(20), "State-only metadata submission stopped client");
    }
    const auto after = app.GetUiPresentation(load);
    Require(after.first_submission == before.first_submission &&
                after.last_submission == before.last_submission &&
                after.submitted_count == before.submitted_count && observer.callbacks == callbacks,
            "State-only commit acquired a pixel/UI submission identity");
    Require(app.GetRenderStats().surface_state_commits == stats.surface_state_commits + 1,
            "Blur metadata did not produce exactly one state commit");
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

void CheckSupersededSubmission(const std::string &socket, const std::string &assets)
{
    Application app(Config(socket, assets, "prepared_superseded_submission"));
    const auto base = app.BeginUiLoad();
    const auto initial = PrepareOnWorker(R"(Card { Text("Base") })", "submission-base");
    Require(app.OpenPrepared(base, initial), "Superseded submission client did not open");
    WaitForUi(app, base, 0);
    WaitForSettledUi(app, base);
    const auto swaps_before = app.GetRenderStats().swap_successes;

    const auto prepared_a = PrepareOnWorker(R"(Card { Text("A") })", "submission-a");
    const auto prepared_b = PrepareOnWorker(R"(Card { Text("B") })", "submission-b");
    const auto prepared_c = PrepareOnWorker(R"(Card { Text("C") })", "submission-c");
    const auto a = app.BeginUiLoad();
    Require(app.ReplaceUiPrepared(a, prepared_a), "First rapid replacement failed");

    // Render runs independently. Let A swap while the UI owner deliberately
    // leaves its Submitted event in the reverse queue.
    std::this_thread::sleep_for(300ms);
    Require(!app.GetUiPresentation(a).submitted,
            "Unpumped A submission unexpectedly reached the UI owner");

    const auto b = app.BeginUiLoad();
    Require(app.ReplaceUiPrepared(b, prepared_b), "Second rapid replacement failed");
    const auto c = app.BeginUiLoad();
    Require(app.ReplaceUiPrepared(c, prepared_c), "Third rapid replacement failed");
    Require(!app.GetUiPresentation(a).installed && app.GetUiPresentation(c).installed,
            "Three replacements did not evict the oldest UI identity");

    WaitForUi(app, c, 0);
    Require(app.GetRenderStats().swap_successes >= swaps_before + 2,
            "Old and newest rapid replacements did not both submit pixels");
    Print("superseded-submission-three-replacements", app, c);
    app.Close();
}

class TemporaryImages {
public:
    explicit TemporaryImages(const std::string &assets)
    {
        char directory[] = "/tmp/prism-staged-images.XXXXXX";
        const auto *created = mkdtemp(directory);
        Require(created != nullptr, "Could not create temporary staged assets");
        path_ = created;
        try {
            for (const auto *name : {"first.png", "second.png", "third.png"}) {
                std::filesystem::copy_file(std::filesystem::path(assets) / "checker.png",
                                           path_ / name);
            }
        } catch (...) {
            Cleanup();
            throw;
        }
    }

    ~TemporaryImages()
    {
        Cleanup();
    }

    std::string Root() const
    {
        return path_.string();
    }

private:
    void Cleanup() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::filesystem::path path_;
};

class UiWorkTurn {
public:
    explicit UiWorkTurn(Application &app) : app_(app)
    {
        app_.BeginUiWorkTurn();
    }

    ~UiWorkTurn()
    {
        if (!app_.EndUiWorkTurn()) {
            std::terminate();
        }
    }

private:
    Application &app_;
};

void CheckTurnBudget(const prism::runtime::UiInstallStats &before,
                     const prism::runtime::UiInstallStats &after)
{
    Require(after.nodes - before.nodes <= 1,
            "Staged scene construction exceeded the shared one-node allowance");
}

void WaitForInstallResources(Application &app)
{
    if (app.UiInstallNeedsWork()) {
        return;
    }
    if (app.ConfigureCount() > 0) {
        Require(app.Pump(20), "Staged upload acknowledgment stopped the client");
        return;
    }
    const int completion = app.ResourceCompletionFd();
    Require(completion >= 0, "Staged resource wait has no completion descriptor");
    pollfd descriptor{completion, POLLIN, 0};
    Require(poll(&descriptor, 1, 5000) > 0 && (descriptor.revents & POLLIN),
            "Staged resource completion did not wake its owner");
}

void FinishStagedInstall(Application &app, const prism::runtime::BindingValues &bindings, bool pump)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (app.UiInstallPending()) {
        Require(std::chrono::steady_clock::now() < deadline, "Staged installation timed out");
        LoadDiagnostic diagnostic;
        UiInstallState state;
        const auto before = app.GetUiInstallStats();
        {
            UiWorkTurn turn(app);
            app.PollImageResources();
            state = app.AdvanceUiInstall(bindings, &diagnostic);
            if (pump && state != UiInstallState::Failed && state != UiInstallState::Cancelled) {
                Require(app.Pump(0), "Staged client stopped during an owner turn");
            }
            CheckTurnBudget(before, app.GetUiInstallStats());
        }
        Require(state == UiInstallState::Pending || state == UiInstallState::Committed,
                diagnostic.message.empty() ? "Staged install failed" : diagnostic.message);
        if (state == UiInstallState::Pending) {
            WaitForInstallResources(app);
        }
    }
}

void CheckStagedImageAcknowledgments(const std::string &socket, const std::string &assets)
{
    auto config = Config(socket, assets, "prepared_image_ack");
    config.install_limits.nodes_per_turn = 1;
    config.install_limits.images_per_turn = 1;
    config.install_limits.cpu_per_turn = 50ms;
    Application app(std::move(config));
    const auto live = app.BeginUiLoad();
    const auto plain = PrepareOnWorker(R"(Card { Text("Live") })", "image-ack-live");
    Require(app.OpenPrepared(live, plain), "Image acknowledgment client did not open");
    WaitForUi(app, live, 0);

    const auto image = app.BeginUiLoad();
    const auto prepared =
        PrepareOnWorker(R"(Card { Image("checker.png",width:72,height:72) })", "image-ack-staged");
    Require(app.StartPreparedInstall(image, prepared), "Image acknowledgment stage did not start");

    const auto before = app.GetUiInstallStats().image_uploads;
    const auto registrations = app.GetUiInstallStats().image_registrations;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (app.GetUiInstallStats().image_registrations == registrations) {
        Require(std::chrono::steady_clock::now() < deadline,
                "Staged image registration did not complete");
        UiInstallState state;
        {
            UiWorkTurn turn(app);
            state = app.AdvanceUiInstall({});
        }
        Require(state == UiInstallState::Pending,
                "Staged image committed before its upload acknowledgment");
        if (app.GetUiInstallStats().image_registrations == registrations) {
            WaitForInstallResources(app);
        }
    }

    // The worker may have uploaded the texture already, but UI installation
    // cannot consume its acknowledgment until a reverse-event Pump.
    {
        UiWorkTurn turn(app);
        Require(app.AdvanceUiInstall({}) == UiInstallState::Pending,
                "Staged image committed before its upload acknowledgment was delivered");
    }

    FinishStagedInstall(app, {}, true);
    WaitForUi(app, image, 1);
    const auto uploaded = app.GetUiInstallStats().image_uploads;
    Require(uploaded == before + 1, "Staged image did not upload exactly once");

    const auto cleared = app.BeginUiLoad();
    Require(app.StartPreparedInstall(cleared, plain), "Image release stage did not start");
    FinishStagedInstall(app, {}, true);
    WaitForUi(app, cleared, 1);

    const auto replacement = app.BeginUiLoad();
    Require(app.StartPreparedInstall(replacement, prepared),
            "Image replacement stage did not start");
    FinishStagedInstall(app, {}, true);
    WaitForUi(app, replacement, 2);
    Require(app.GetUiInstallStats().image_uploads == uploaded + 1,
            "Image released then requested again did not receive one new upload");
    Print("staged-image-version-ack-and-release", app, replacement);
    app.Close();
}

void CheckStagedCancellation(Application &app)
{
    const auto live = app.GetRenderStats();
    const auto cancelled = app.BeginUiLoad();
    const auto candidate = PrepareOnWorker(R"(
        VStack {
            Text($candidate)
            Text("Uncommitted candidate")
            Text("Still detached")
        }
    )",
                                           "staged-cancellation");
    Require(app.StartPreparedInstall(cancelled, candidate), "Could not stage cancellation case");
    {
        UiWorkTurn turn(app);
        const auto before = app.GetUiInstallStats();
        Require(app.AdvanceUiInstall({{"candidate", std::string("Not live")}}) ==
                    UiInstallState::Pending,
                "One-node staged candidate was installed before cancellation");
        CheckTurnBudget(before, app.GetUiInstallStats());
        app.CancelUiLoad();
        Require(!app.UiInstallPending() && !app.GetUiPresentation(cancelled).installed,
                "Cancellation published or retained a staged candidate");
        Require(app.SetBinding("next", std::string("Next staged image")) &&
                    !app.SetBinding("candidate", std::string("Must stay detached")),
                "Partial candidate cancellation changed live binding targets");
        Require(app.Pump(0), "Live client stopped after staged cancellation");
    }
    NoWork(live, app.GetRenderStats(), "Staged cancellation rendered detached pixels");

    const auto rejected = app.BeginUiLoad();
    const auto invalid = PrepareOnWorker(R"(Card(material:"missing-material") { Text($invalid) })",
                                         "staged-invalid-material");
    Require(app.StartPreparedInstall(rejected, invalid), "Could not stage invalid material");
    LoadDiagnostic diagnostic;
    {
        UiWorkTurn turn(app);
        Require(app.AdvanceUiInstall({}, &diagnostic) == UiInstallState::Failed &&
                    diagnostic.stage == LoadStage::Install && !diagnostic.message.empty(),
                "Invalid material did not reject the detached candidate");
        Require(!app.GetUiPresentation(rejected).installed &&
                    app.SetBinding("next", std::string("Next staged image")) &&
                    !app.SetBinding("invalid", std::string("Must stay detached")),
                "Rejected material transaction changed the live UI/bindings");
        Require(app.Pump(0), "Live client stopped after invalid staged material");
    }
    NoWork(live, app.GetRenderStats(), "Rejected material rendered detached pixels");
    app.CancelUiLoad();
}

void CheckStagedOwnerLedger(const std::string &socket, const std::string &assets)
{
    TemporaryImages images(assets);
    auto config = Config(socket, images.Root(), "prepared_staged_owner_ledger");
    config.install_limits.nodes_per_turn = 1;
    config.install_limits.images_per_turn = 1;
    config.install_limits.upload_bytes_per_turn = 1;
    config.install_limits.cpu_per_turn = 2ms;
    Application app(std::move(config));
    Require(app.ApplyTheme(Theme()), "Staged probe theme was rejected");
    const auto first = app.BeginUiLoad();
    const auto initial = PrepareOnWorker(R"(
        VStack {
            Text($original)
            HStack {
                Image("first.png",width:72,height:72)
                Image("second.png",width:72,height:72)
                Image("third.png",width:72,height:72)
            }
        }
    )",
                                         "staged-three-image-master");
    Require(app.StartPreparedInstall(first, initial), "Could not stage master-only UI");
    FinishStagedInstall(app, {{"original", std::string("Staged retained UI")}}, false);
    Require(app.GetUiPresentation(first).installed && !app.GetUiPresentation(first).submitted &&
                app.LoadedImageCount() == 3 && app.GetUiInstallStats().image_uploads == 0,
            "Master-only installation uploaded/submitted before Wayland configure");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (app.ConfigureCount() == 0) {
        Require(std::chrono::steady_clock::now() < deadline, "Staged window configure timed out");
        UiWorkTurn turn(app);
        Require(app.Pump(5), "Staged window failed before first configure");
    }
    Require(app.ConfigureCount() > 0, "Master-only UI missed its first configure");
    if (app.GetUiPresentation(first).submitted) {
        Require(app.GetUiInstallStats().image_uploads == 3,
                "Master-only UI submitted before all three images uploaded");
    }

    const auto replacement = PrepareOnWorker(R"(
        VStack {
            Text($next)
            Image("second.png",width:72,height:72)
        }
    )",
                                             "staged-post-pump-replacement");
    // Uploads run on the independent render worker. UI may observe one or
    // several worker turns together, so only cumulative versions are tested.
    while (app.GetUiInstallStats().image_uploads == 0) {
        Require(std::chrono::steady_clock::now() < deadline, "Initial staged upload timed out");
        Require(app.Pump(20), "Staged client stopped during worker image upload");
    }
    const auto first_upload = app.GetUiInstallStats();
    Require(first_upload.oversized_uploads > 0 && first_upload.upload_bytes > 1,
            "Oversized atomic image upload was not recorded");

    const auto next = app.BeginUiLoad();
    {
        const auto before = app.GetUiInstallStats();
        UiWorkTurn turn(app);
        Require(app.StartPreparedInstall(next, replacement), "Could not stage post-Pump UI");
        Require(app.AdvanceUiInstall({{"next", std::string("Next staged image")}}) ==
                    UiInstallState::Pending,
                "Post-Pump replacement bypassed its shared work allowance");
        Require(app.SetBinding("original", std::string("Staged retained UI")),
                "Post-Pump candidate prematurely replaced live bindings");
        Require(app.Pump(0), "Second Pump in the same scoped turn stopped the client");
        CheckTurnBudget(before, app.GetUiInstallStats());
        Require(!app.GetUiPresentation(next).installed,
                "Exhausted UI owner turn installed replacement UI");
    }
    FinishStagedInstall(app, {{"next", std::string("Next staged image")}}, true);
    Require(app.SetBinding("next", std::string("Next staged image")) &&
                !app.SetBinding("original", std::string("Obsolete")),
            "Committed staged replacement retained old binding targets");
    WaitForUi(app, next, 3);
    WaitForSettledUi(app, next);
    Require(app.GetUiInstallStats().image_uploads == 3 &&
                app.GetUiInstallStats().oversized_uploads == 3,
            "Independent worker did not upload three image versions exactly once");
    Require(app.GlRenderer().find("V3D") != std::string::npos,
            "Staged upload/presentation did not use the real V3D driver");
    CheckStagedCancellation(app);
    Print("staged-owner-turn-upload-ledger-and-cancellation", app, next);
    const auto stats = app.GetUiInstallStats();
    std::cout << "staged_uploads=" << stats.image_uploads << " staged_bytes=" << stats.upload_bytes
              << " oversized_atomic_uploads=" << stats.oversized_uploads
              << " shared_upload_limit=1 shared_nodes_limit=1\n";
    app.Close();
}

int Verify(const std::string &socket, const std::string &assets)
{
    CheckOpenRetry(socket, assets);
    CheckSupersededSubmission(socket, assets);
    CheckStagedImageAcknowledgments(socket, assets);
    CheckStagedOwnerLedger(socket, assets);

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
