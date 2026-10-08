#include "app_host_p.hpp"
#include "client_application_p.hpp"
#include "module_feedback_p.hpp"
#include "owner_task_native_p.hpp"
#include "prism/runtime/task_paint.hpp"
#include "scene_p.hpp"

#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <tuple>

namespace {
using namespace prism::tests::owner_task_probe;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Snapshot = prism::runtime::InputSnapshot;

std::string Read(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

struct NativeFocus {
    std::uint64_t seat{};
    prism::contracts::NodeId node{};
    bool operator==(const NativeFocus &) const = default;
};

bool PaintAdopted(const prism::sdk::ClientApplication::Impl &app,
                  const prism::runtime::FramePacket &frame)
{
    const auto &paint = app.owner_task_paint;
    if (!paint || paint != frame.task_paint_candidate || !frame.task_presentation ||
        paint->commands.empty()) {
        return false;
    }

    const auto &source = paint->source;
    const auto &stamp = *frame.task_presentation;
    return source.identity == stamp.identity && source.identity.ui == app.installed_ui &&
           source.projection == stamp.projection && source.frame_sequence == frame.sequence &&
           source.configure_count == frame.configure_count &&
           frame.configure_count == app.ui_configure_count &&
           source.buffer_size.width == frame.buffer_size.width &&
           source.buffer_size.height == frame.buffer_size.height &&
           frame.buffer_size.width == app.ui_metrics.buffer_size.width &&
           frame.buffer_size.height == app.ui_metrics.buffer_size.height &&
           source.scale == frame.scale && frame.scale == app.ui_metrics.scale &&
           source.theme_generation == frame.theme_generation &&
           frame.theme_generation == (app.theme ? app.theme->generation : 0) &&
           source.resource_epoch == frame.resource_epoch &&
           frame.resource_epoch == app.commands.ResourceEpoch();
}

struct MotionAdoption {
    std::uint64_t owner{}, request{}, ui_owner{}, ui_generation{}, cycle{}, projection{},
        frame_sequence{}, generation{}, revision{}, time_ns{};
    double reveal{};
    std::string endpoint, sample_kind, phase, observation;
    bool task_ready{}, input_retired{}, retained_paint{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MotionAdoption, owner, request, ui_owner, ui_generation, cycle,
                                   projection, frame_sequence, generation, revision, time_ns,
                                   reveal, endpoint, sample_kind, phase, observation, task_ready,
                                   input_retired, retained_paint)

struct MotionEvidence {
    std::vector<MotionAdoption> adopted;
    std::uint64_t opening_partial_adoptions{}, closing_partial_adoptions{},
        open_terminal_adoptions{}, closed_terminal_adoptions{}, complete_cycles{};
    bool nonzero_transition_verified{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MotionEvidence, adopted, opening_partial_adoptions,
                                   closing_partial_adoptions, open_terminal_adoptions,
                                   closed_terminal_adoptions, complete_cycles,
                                   nonzero_transition_verified)

MotionEvidence Evidence(const std::vector<MotionAdoption> &adopted)
{
    MotionEvidence evidence;
    evidence.adopted = adopted;
    using Track = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>;
    using Cycle = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    std::map<Track, unsigned> tracks;
    std::map<Cycle, unsigned> cycles;
    for (const auto &sample : adopted) {
        auto &track = tracks[{sample.owner, sample.request, sample.cycle, sample.generation}];
        const bool terminal = sample.sample_kind == "Terminal";
        const bool partial = !terminal && sample.reveal > 0 && sample.reveal < 1;
        if (sample.endpoint == "Open") {
            evidence.opening_partial_adoptions += partial;
            evidence.open_terminal_adoptions += terminal;
            track |= partial ? 1u : terminal ? 2u : 0u;
        } else {
            evidence.closing_partial_adoptions += partial;
            evidence.closed_terminal_adoptions += terminal;
            track |= partial ? 4u : terminal ? 8u : 0u;
        }
    }
    for (const auto &[track, flags] : tracks) {
        auto &cycle = cycles[{std::get<0>(track), std::get<1>(track), std::get<2>(track)}];
        if ((flags & 3u) == 3u) {
            cycle |= 1u;
        }
        if ((flags & 12u) == 12u) {
            cycle |= 2u;
        }
    }
    for (const auto &[cycle, flags] : cycles) {
        evidence.complete_cycles += flags == 3u;
    }
    evidence.nonzero_transition_verified = evidence.complete_cycles > 0;
    return evidence;
}

bool MotionAdopted(const prism::sdk::ClientApplication::Impl &app,
                   const prism::runtime::FramePacket &frame)
{
    if (!frame.task_motion || !frame.task_presentation) {
        return false;
    }
    const auto &motion = *frame.task_motion;
    const auto &stamp = *frame.task_presentation;
    if (!motion.generation || !motion.revision || motion.identity != stamp.identity ||
        motion.sample.endpoint != stamp.endpoint || motion.sample.kind != stamp.sample_kind ||
        !std::isfinite(motion.sample.reveal) || motion.sample.reveal < 0 ||
        motion.sample.reveal > 1) {
        return false;
    }
    if (stamp.endpoint == prism::runtime::TaskPresentationEndpoint::Closed &&
        stamp.sample_kind == prism::runtime::TaskPresentationSampleKind::Terminal) {
        return motion.sample.reveal == 0 && !app.owner_task_motion;
    }
    return app.owner_task_motion && app.owner_task_motion->adopted &&
           *app.owner_task_motion->adopted == motion &&
           app.owner_task_motion->current.generation == motion.generation;
}

bool TerminalMotionAdopted(const prism::sdk::ClientApplication::Impl &app,
                           const prism::runtime::FramePacket &frame)
{
    if (!frame.task_motion) {
        return !app.owner_task_motion;
    }
    return MotionAdopted(app, frame) &&
           frame.task_motion->sample.kind == prism::runtime::TaskPresentationSampleKind::Terminal &&
           frame.task_motion->sample.reveal ==
               (frame.task_motion->sample.endpoint == prism::runtime::TaskPresentationEndpoint::Open
                    ? 1
                    : 0);
}

const char *PhaseName(prism::runtime::TaskPresentationPhase phase)
{
    switch (phase) {
    case prism::runtime::TaskPresentationPhase::Opening:
        return "Opening";
    case prism::runtime::TaskPresentationPhase::Open:
        return "Open";
    case prism::runtime::TaskPresentationPhase::Closing:
        return "Closing";
    case prism::runtime::TaskPresentationPhase::Closed:
        return "Closed";
    }
    return "Invalid";
}

void DumpMotion(std::string_view label,
                const std::optional<prism::runtime::TaskMotionFrameStamp> &motion)
{
    if (!motion) {
        std::cerr << ' ' << label << "=none";
        return;
    }
    const auto &sample = motion->sample;
    std::cerr << ' ' << label << "[generation=" << motion->generation
              << ",revision=" << motion->revision << ",time_ns=" << sample.time_ns
              << ",reveal=" << sample.reveal << ",kind=" << static_cast<int>(sample.kind)
              << ",endpoint=" << static_cast<int>(sample.endpoint) << ']';
}

void DumpPaint(std::string_view label,
               const std::shared_ptr<const prism::runtime::TaskPaintFragment> &paint)
{
    if (!paint) {
        std::cerr << ' ' << label << "=none";
        return;
    }

    const auto &source = paint->source;
    std::cerr << ' ' << label << "[owner=" << source.identity.task.owner.value
              << ",request=" << source.identity.task.request.value
              << ",ui=" << source.identity.ui.owner << ':' << source.identity.ui.generation
              << ",cycle=" << source.identity.cycle << ",projection=" << source.projection
              << ",sequence=" << source.frame_sequence << ",commands=" << paint->commands.size()
              << ",configure=" << source.configure_count << ",buffer=" << source.buffer_size.width
              << 'x' << source.buffer_size.height << ",scale=" << source.scale
              << ",theme=" << source.theme_generation << ",resources=" << source.resource_epoch
              << ']';
}

class Owner {
public:
    Owner(const char *socket, const std::filesystem::path &package,
          const std::filesystem::path &source, std::uint64_t instance)
        : host(Config(socket, source, instance))
    {
        const auto loaded = prism::launch::LoadPackage(package);
        app_id = loaded.manifest.app_id;
        Require(host.Bind(loaded), "Notepad native Host Bind failed");
    }

    static prism::sdk::HostConfig Config(const char *socket, const std::filesystem::path &source,
                                         std::uint64_t instance)
    {
        prism::sdk::HostConfig config;
        config.socket = socket;
        config.request = {instance};
        config.instance = {instance};
        config.initial_theme =
            prism::theme::LoadTheme(source / "resources/themes", "square", 1, "light");
        return config;
    }

    prism::sdk::ClientApplication &Frontend()
    {
        Require(bool(host.impl_->frontend), "Notepad native Host has no frontend");
        return *host.impl_->frontend;
    }

    std::string String(std::string_view key) const
    {
        const auto at = host.impl_->bindings.find(std::string(key));
        if (at == host.impl_->bindings.end()) {
            return {};
        }
        const auto *value = std::get_if<std::string>(&at->second);
        Require(value, "Expected string business binding");
        return *value;
    }

    bool Boolean(std::string_view key) const
    {
        const auto at = host.impl_->bindings.find(std::string(key));
        if (at == host.impl_->bindings.end()) {
            return false;
        }
        const auto *value = std::get_if<bool>(&at->second);
        Require(value, "Expected bool business binding");
        return *value;
    }

    std::shared_ptr<const Snapshot> Adopted() const
    {
        const auto &app = *host.impl_->frontend->impl_;
        const auto &frame = app.ui_root_metadata_frame;
        if (!frame || frame->ui != app.installed_ui || !frame->input_snapshot || !app.scene ||
            !app.scene->IsInputSnapshotAdopted(*frame->input_snapshot)) {
            return {};
        }
        return frame->input_snapshot;
    }

    bool Ready() const
    {
        const auto task = host.impl_->frontend->ActiveOwnerTask();
        const auto presentation = host.impl_->frontend->OwnerTaskPresentation();
        const auto &app = *host.impl_->frontend->impl_;
        const auto &frame = app.ui_root_metadata_frame;
        const auto input = Adopted();
        return task && task->phase == prism::runtime::TaskPhase::Ready && input &&
               input->owner_modal_epoch &&
               input->owner_modal_epoch == app.scene->OwnerModalEpoch() && presentation &&
               presentation->phase == prism::runtime::TaskPresentationPhase::Open &&
               presentation->identity.task == task->identity && frame && frame->task_presentation &&
               frame->task_presentation->endpoint ==
                   prism::runtime::TaskPresentationEndpoint::Open &&
               frame->task_presentation->sample_kind ==
                   prism::runtime::TaskPresentationSampleKind::Terminal &&
               presentation->identity == frame->task_presentation->identity &&
               presentation->projection == frame->task_presentation->projection &&
               presentation->binding == frame->task_presentation->binding &&
               presentation->adopted_sequence == frame->sequence &&
               frame->task_presentation->frame_sequence == frame->sequence &&
               frame->task_presentation->binding.input_scene == input->scene &&
               frame->task_presentation->binding.input_version == input->version &&
               PaintAdopted(app, *frame) && TerminalMotionAdopted(app, *frame);
    }

    bool PresentationClosed() const
    {
        const auto &app = *host.impl_->frontend->impl_;
        const auto state = host.impl_->frontend->OwnerTaskPresentation();
        if (host.impl_->frontend->ActiveOwnerTask() || app.owner_task_scope ||
            host.impl_->owner_task || app.owner_task_paint || !app.scene ||
            app.scene->OwnerModalToken() || !Adopted()) {
            return false;
        }
        const auto &frame = app.ui_root_metadata_frame;
        if (!state) {
            return !frame->task_presentation;
        }
        if (state->phase != prism::runtime::TaskPresentationPhase::Closed) {
            return false;
        }
        // A later ordinary/Saved feedback frame can supersede the adopted Closed
        // endpoint. When that endpoint is still present, verify its exact receipt.
        if (!frame->task_presentation) {
            return true;
        }
        const auto &stamp = *frame->task_presentation;
        return stamp.endpoint == prism::runtime::TaskPresentationEndpoint::Closed &&
               stamp.sample_kind == prism::runtime::TaskPresentationSampleKind::Terminal &&
               state->identity == stamp.identity && state->projection == stamp.projection &&
               state->binding == stamp.binding && state->adopted_sequence == frame->sequence &&
               stamp.frame_sequence == frame->sequence && TerminalMotionAdopted(app, *frame);
    }

    void Pump()
    {
        if (!host.Pump(10)) {
            Dump();
            Require(false, "Notepad native Host stopped unexpectedly");
        }
        ObserveMotion();
    }

    void InstallMotionObserver()
    {
        previous_submitted = std::move(Frontend().impl_->on_ui_submitted);
        Frontend().OnUiSubmitted(std::bind_front(&Owner::ObserveSubmitted, this));
    }

    void ObserveSubmitted(prism::runtime::UiLoadId ui)
    {
        if (host.impl_->frontend && ui == host.impl_->frontend->impl_->installed_ui) {
            ObserveMotion("AtPixelsCallback");
        }
        if (previous_submitted) {
            previous_submitted(ui);
        }
    }

    void ObserveMotion(std::string_view observation = "AdoptedMetadataAfterPump")
    {
        if (!host.impl_->frontend) {
            return;
        }
        const auto &app = *host.impl_->frontend->impl_;
        const auto &frame = app.ui_root_metadata_frame;
        const auto state = host.impl_->frontend->OwnerTaskPresentation();
        const auto input = Adopted();
        if (!frame || !frame->task_motion || !frame->task_presentation || !state || !input ||
            state->identity != frame->task_presentation->identity ||
            state->projection != frame->task_presentation->projection ||
            state->binding != frame->task_presentation->binding ||
            state->adopted_sequence != frame->sequence ||
            frame->task_presentation->frame_sequence != frame->sequence ||
            frame->task_presentation->binding.input_scene != input->scene ||
            frame->task_presentation->binding.input_version != input->version ||
            frame->configure_count != app.ui_configure_count ||
            frame->buffer_size.width != app.ui_metrics.buffer_size.width ||
            frame->buffer_size.height != app.ui_metrics.buffer_size.height ||
            frame->scale != app.ui_metrics.scale ||
            frame->theme_generation != (app.theme ? app.theme->generation : 0) ||
            frame->resource_epoch != app.commands.ResourceEpoch() || !MotionAdopted(app, *frame)) {
            return;
        }
        if (!motion_adoptions.empty() &&
            motion_adoptions.back().frame_sequence == frame->sequence) {
            return;
        }

        const auto &motion = *frame->task_motion;
        const auto &sample = motion.sample;
        const auto active = host.impl_->frontend->ActiveOwnerTask();
        const bool retired = !active && !app.owner_task_scope && !app.scene->OwnerModalToken();
        const bool closing = sample.endpoint == prism::runtime::TaskPresentationEndpoint::Closed;
        const bool terminal = sample.kind == prism::runtime::TaskPresentationSampleKind::Terminal;
        const auto expected_phase = closing    ? terminal
                                                     ? prism::runtime::TaskPresentationPhase::Closed
                                                     : prism::runtime::TaskPresentationPhase::Closing
                                    : terminal ? prism::runtime::TaskPresentationPhase::Open
                                               : prism::runtime::TaskPresentationPhase::Opening;
        Require(state->phase == expected_phase,
                "Actual motion adoption settled an Intermediate or failed to settle its Terminal");
        Require(closing ||
                    (app.scene->owner_modal_ && app.scene->owner_modal_->input_ready == terminal),
                "Opening motion input gate disagrees with actual Terminal adoption");
        Require(!closing || retired, "Closing motion retained task input or business state");
        Require(!closing || sample.kind == prism::runtime::TaskPresentationSampleKind::Terminal ||
                    bool(app.owner_task_paint),
                "Adopted Closing intermediate released its retained paint prematurely");
        Require(closing || PaintAdopted(app, *frame),
                "Adopted opening motion has no matching retained paint source");
        motion_adoptions.push_back(
            {motion.identity.task.owner.value, motion.identity.task.request.value,
             motion.identity.ui.owner, motion.identity.ui.generation, motion.identity.cycle,
             frame->task_presentation->projection, frame->sequence, motion.generation,
             motion.revision, sample.time_ns, sample.reveal, closing ? "Closed" : "Open",
             sample.kind == prism::runtime::TaskPresentationSampleKind::Terminal ? "Terminal"
                                                                                 : "Intermediate",
             PhaseName(state->phase), std::string(observation),
             active && active->phase == prism::runtime::TaskPhase::Ready, retired,
             bool(app.owner_task_paint)});
    }

    void Dump() const
    {
        std::cerr << "notepad native diagnostic status=" << String("status")
                  << " details=" << String("details") << " busy=" << Boolean("busy")
                  << " failure=" << host.GetUiState().failed;
        if (host.impl_->frontend) {
            const auto &app = *host.impl_->frontend->impl_;
            const auto presentation = host.impl_->frontend->OwnerTaskPresentation();
            std::cerr << " scope=" << bool(app.owner_task_scope)
                      << " host_pending=" << bool(host.impl_->owner_task)
                      << " presentation=" << bool(presentation);
            if (presentation) {
                std::cerr << " presentation[phase=" << static_cast<int>(presentation->phase)
                          << ",owner=" << presentation->identity.task.owner.value
                          << ",request=" << presentation->identity.task.request.value
                          << ",ui=" << presentation->identity.ui.owner << ':'
                          << presentation->identity.ui.generation
                          << ",cycle=" << presentation->identity.cycle
                          << ",projection=" << presentation->projection
                          << ",adopted_sequence=" << presentation->adopted_sequence << ']';
            }
            DumpPaint("retained_paint", app.owner_task_paint);
            if (app.owner_task_motion) {
                DumpMotion("motion_current", app.owner_task_motion->current);
                DumpMotion("motion_adopted", app.owner_task_motion->adopted);
            }
            if (const auto &frame = app.ui_root_metadata_frame) {
                std::cerr << " metadata[sequence=" << frame->sequence << ",ui=" << frame->ui.owner
                          << ':' << frame->ui.generation << ",configure=" << frame->configure_count
                          << ",theme=" << frame->theme_generation << ']';
                DumpPaint("paint_candidate", frame->task_paint_candidate);
                DumpMotion("motion_frame", frame->task_motion);
                if (const auto &input = frame->input_snapshot) {
                    std::cerr << " input[scene=" << input->scene << ",version=" << input->version
                              << ",epoch=" << input->owner_modal_epoch
                              << ",adopted=" << bool(Adopted()) << ']';
                }
                if (const auto &stamp = frame->task_presentation) {
                    std::cerr << " tag[endpoint=" << static_cast<int>(stamp->endpoint)
                              << ",sample_kind=" << static_cast<int>(stamp->sample_kind)
                              << ",owner=" << stamp->identity.task.owner.value
                              << ",request=" << stamp->identity.task.request.value
                              << ",cycle=" << stamp->identity.cycle
                              << ",projection=" << stamp->projection
                              << ",sequence=" << stamp->frame_sequence
                              << ",scene=" << stamp->binding.input_scene
                              << ",input_version=" << stamp->binding.input_version
                              << ",token=" << stamp->binding.modal_token
                              << ",epoch=" << stamp->binding.modal_epoch << ']';
                } else {
                    std::cerr << " tag=none";
                }
            } else {
                std::cerr << " metadata=none";
            }
        }
        std::cerr << '\n';
    }

    std::vector<NativeFocus> FocusSnapshot()
    {
        std::vector<NativeFocus> result;
        for (const auto &focus : Frontend().impl_->scene->input_state_->focus) {
            if (focus.seat && focus.node && Frontend().impl_->scene->Find(focus.node)) {
                result.push_back({focus.seat, focus.node});
            }
        }
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
            return std::tie(a.seat, a.node.index, a.node.generation) <
                   std::tie(b.seat, b.node.index, b.node.generation);
        });
        return result;
    }

    bool ObserveFeedback(const prism::contracts::OwnerFeedbackRequest &request)
    {
        // Observe the real sink boundary, after pointer-driven focus changes
        // and before SDK presentation. This leaves all ABI and Host routing live.
        const auto before = FocusSnapshot();
        const bool accepted = host.impl_->ShowOwnerFeedback(request);
        if (accepted) {
            ++feedback_focus_checks;
            feedback_focus_samples += !before.empty();
            feedback_focus_changes += before != FocusSnapshot();
        }
        return accepted;
    }

    bool ObserveClose()
    {
        ++close_receipts;
        return host.impl_->HandleCloseRequested();
    }

    prism::sdk::AppHost host;
    std::string app_id;
    std::function<void(prism::runtime::UiLoadId)> previous_submitted;
    std::vector<MotionAdoption> motion_adoptions;
    std::uint64_t close_receipts{}, feedback_focus_checks{}, feedback_focus_samples{},
        feedback_focus_changes{};
};

template <typename Predicate> void Wait(Owner &owner, Predicate ready, std::string_view detail)
{
    const auto deadline = Clock::now() + 8s;
    while (!ready()) {
        if (Clock::now() >= deadline) {
            owner.Dump();
            Require(false, detail);
        }
        owner.Pump();
    }
}

void Started(Owner &owner)
{
    Wait(
        owner,
        [&owner] { return owner.host.GetUiState().master_presented && bool(owner.Adopted()); },
        "Notepad Master never reached real presentation/input adoption");
    Require(owner.Frontend().GlRenderer().find("V3D") != std::string::npos,
            "Notepad native gate requires hardware V3D");
    Require(owner.Frontend().SupportsOwnerFileTasks(), "Notepad shared file panel unavailable");
    owner.InstallMotionObserver();
    owner.Frontend().OnCloseRequested(std::bind_front(&Owner::ObserveClose, &owner));
    Require(owner.Frontend().SupportsOwnerFeedback(), "Notepad shared feedback unavailable");
    owner.host.impl_->business->feedback_->submit =
        std::bind_front(&Owner::ObserveFeedback, &owner);
}

Point ActionPoint(Owner &owner, std::string_view action)
{
    const auto deadline = Clock::now() + 8s;
    for (;;) {
        const auto input = owner.Adopted();
        const auto tree = Tree();
        std::optional<Rect> global;
        for (const auto &workspace : tree.workspaces) {
            if (const auto bounds = FindOwner(workspace, owner.app_id)) {
                Require(!global, "Notepad app identity matched two native views");
                global = bounds;
            }
        }
        if (input && global && std::abs(global->width - input->viewport.width) < 1 &&
            std::abs(global->height - input->viewport.height) < 1) {
            for (const auto &node : input->nodes) {
                if (node.visible && node.enabled && node.action == action && node.interactive) {
                    const Point point{node.bounds.x + node.bounds.width / 2,
                                      node.bounds.y + node.bounds.height / 2};
                    const auto hit = owner.Frontend().impl_->scene->HitTest(point, *input);
                    if (hit && hit->node == node.id) {
                        return {global->x + point.x, global->y + point.y};
                    }
                }
            }
        }
        if (Clock::now() >= deadline) {
            owner.Dump();
            Require(false,
                    std::string("Native Notepad action not hittable: ") + std::string(action));
        }
        owner.Pump();
    }
}

bool Focused(Owner &owner, std::string_view action)
{
    const auto &scene = *owner.Frontend().impl_->scene;
    for (const auto &focus : scene.input_state_->focus) {
        const auto *node = scene.Find(focus.node);
        if (focus.seat && node && node->action == action) {
            return true;
        }
    }
    return false;
}

void Type(Owner &owner, Input &input, std::string_view action, std::string_view text)
{
    input.Click(ActionPoint(owner, action));
    Wait(
        owner, [&owner, action] { return Focused(owner, action); },
        "Native Notepad editor did not receive the real input seat focus");
    input.SelectAll();
    for (const auto character : text) {
        input.Type(character);
        owner.Pump();
    }
}

void Restored(Owner &owner)
{
    Wait(
        owner,
        [&owner] {
            return !owner.Frontend().ActiveOwnerTask() && owner.Adopted() &&
                   !owner.Frontend().impl_->scene->OwnerModalToken() && owner.PresentationClosed();
        },
        "Notepad did not adopt Closed presentation and restore normal task input");
}

void FileReady(Owner &owner)
{
    Wait(
        owner,
        [&owner] {
            return owner.Ready() && owner.host.impl_->owner_task &&
                   owner.host.impl_->owner_task->file &&
                   owner.host.impl_->owner_task->file->phase ==
                       prism::sdk::OwnerFileState::Phase::Browsing;
        },
        "Notepad file task never reached adopted Browsing");
}

std::uint64_t FeedbackReady(Owner &owner, prism::contracts::OwnerFeedbackKind kind)
{
    Wait(
        owner,
        [&owner, kind] {
            const auto &app = *owner.Frontend().impl_;
            const auto input = owner.Adopted();
            if (!app.owner_feedback || app.owner_feedback->kind != kind || !input ||
                app.owner_feedback_hidden || app.owner_feedback_wait_adoption ||
                app.owner_feedback_ui != app.installed_ui ||
                !app.owner_feedback_session.Adopted()) {
                return false;
            }
            const auto card = app.scene->RegionId(prism::runtime::kOwnerFeedbackCardRegion);
            const auto *node = card ? input->Find(card) : nullptr;
            if (!node || !node->visible || node->bounds.width <= 0 || node->bounds.height <= 0) {
                return false;
            }
            const auto dismiss = prism::runtime::OwnerFeedbackActionName(
                app.owner_feedback_generation, app.owner_feedback->request_id, 0);
            return std::any_of(input->nodes.begin(), input->nodes.end(),
                               [&dismiss](const auto &entry) {
                                   return entry.visible && entry.enabled && entry.interactive &&
                                          entry.action == dismiss;
                               });
        },
        "Notepad feedback never reached actual input adoption");
    Require(!owner.feedback_focus_changes, "SDK feedback changed the actual native seat focus");
    return owner.Frontend().impl_->owner_feedback->request_id;
}

std::string FeedbackActionName(Owner &owner, std::uint32_t action)
{
    const auto &app = *owner.Frontend().impl_;
    Require(app.owner_feedback.has_value(), "Native feedback action has no active request");
    return prism::runtime::OwnerFeedbackActionName(app.owner_feedback_generation,
                                                   app.owner_feedback->request_id, action);
}

std::string_view RowAction(Owner &owner, std::string_view name)
{
    const auto &file = *owner.host.impl_->owner_task->file;
    Require(file.listing && file.listing->directory, "Native Notepad directory not available");
    for (std::size_t row = 0; row < file.items.size(); ++row) {
        if (file.listing->directory->entries.at(file.items[row]).name == name) {
            return prism::runtime::kOwnerFileRowActions.at(row);
        }
    }
    Require(false, "Native Notepad fixture file missing from first page");
    return {};
}

void SelectFile(Owner &owner, Input &input, std::string_view name)
{
    input.Click(ActionPoint(owner, RowAction(owner, name)));
    Wait(
        owner,
        [&owner, name] {
            const auto &file = *owner.host.impl_->owner_task->file;
            return owner.Ready() && file.selected &&
                   file.listing->directory->entries.at(*file.selected).name == name;
        },
        "File selection not adopted");
    // ActionPoint waits for a visible enabled submit target in the adopted frame.
    input.Click(ActionPoint(owner, prism::runtime::kOwnerFileSubmitAction));
}

void CloseRequest()
{
    const auto *runtime = std::getenv("XDG_RUNTIME_DIR");
    const auto path = (std::filesystem::path(runtime) / "prism-ipc.sock").string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    Require(path.size() < sizeof(address.sun_path), "Isolated close IPC path too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    Descriptor fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    const timeval timeout{1, 0};
    Require(setsockopt(fd.Get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
                connect(fd.Get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
            "Cannot connect isolated WM close IPC");
    constexpr std::string_view command = "close\n";
    Require(write(fd.Get(), command.data(), command.size()) == static_cast<ssize_t>(command.size()),
            "Cannot request real xdg_toplevel close");
    std::string reply;
    std::array<char, 512> buffer;
    for (;;) {
        const auto count = read(fd.Get(), buffer.data(), buffer.size());
        Require(count >= 0, "Isolated close IPC timed out");
        if (!count) {
            break;
        }
        reply.append(buffer.data(), static_cast<std::size_t>(count));
        Require(reply.size() <= 4096, "Isolated close IPC reply too large");
    }
    Require(nlohmann::json::parse(reply).get<prism::ipc::CommandReply>().status == "ok",
            "Isolated WM had no focused Notepad for close");
}

void Confirmation(Owner &owner)
{
    Wait(
        owner,
        [&owner] {
            return owner.Ready() && owner.host.impl_->owner_task &&
                   owner.host.impl_->owner_task->request.kind ==
                       prism::contracts::OwnerTaskKind::Confirmation;
        },
        "Notepad unsaved confirmation never reached actual input adoption");
}

void Closed(Owner &owner)
{
    const auto deadline = Clock::now() + 8s;
    while (owner.host.Pump(10)) {
        owner.ObserveMotion();
        if (Clock::now() >= deadline) {
            owner.Dump();
            Require(false, "Notepad asynchronous close did not continue automatically");
        }
    }
    Require(owner.host.IsCloseRequested() && !owner.host.GetUiState().failed,
            "Notepad stopped for a failure rather than accepted close");
    Require(owner.Frontend().impl_->bridge->terminal.Reason() ==
                prism::runtime::TerminalReason::None,
            "Notepad accepted close alongside a render worker failure");
}

struct CaseReport {
    std::string name;
    bool passed{}, native_pointer{}, native_keyboard{}, actual_business_io{}, real_wm_close{};
    bool feedback_focus_unchanged{}, feedback_focus_preserved{}, feedback_continued_editing{},
        feedback_recovery_retained{}, feedback_close_retired{}, sdk_focus_feedback_adopted{},
        sdk_existing_focus_preserved{}, sdk_keyboard_without_refocus{};
    std::uint64_t adopted_tasks{}, pixel_commits{}, native_close_events{}, adopted_feedback{},
        native_feedback_actions{}, feedback_focus_checks{}, feedback_focus_samples{};
    MotionEvidence motion;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CaseReport, name, passed, native_pointer, native_keyboard,
                                   actual_business_io, real_wm_close, adopted_tasks, pixel_commits,
                                   native_close_events, adopted_feedback, native_feedback_actions,
                                   feedback_focus_unchanged, feedback_focus_preserved,
                                   feedback_continued_editing, feedback_recovery_retained,
                                   feedback_close_retired, feedback_focus_checks,
                                   feedback_focus_samples, sdk_focus_feedback_adopted,
                                   sdk_existing_focus_preserved, sdk_keyboard_without_refocus,
                                   motion)

void VerifySdkFeedbackFocus(Owner &owner, Input &input, const std::filesystem::path &directory,
                            CaseReport &report)
{
    const auto before = owner.FocusSnapshot();
    Require(!before.empty() && Focused(owner, "edit:1"),
            "SDK focus sample requires a live editor focused by the native input seat");

    // This is a scoped SDK request, not an additional Notepad business result.
    // The real Saved result and subsequent native editing were checked above.
    const prism::contracts::OwnerFeedbackRequest request{
        std::numeric_limits<std::uint64_t>::max(),
        prism::contracts::OwnerFeedbackKind::Info,
        "SDK focus verification",
        "Test feedback for native focus preservation. No file operation is represented.",
        {},
        4000};
    Require(owner.ObserveFeedback(request), "Native SDK focus feedback was rejected");
    Require(owner.FocusSnapshot() == before,
            "SDK Show changed an existing native seat or live editor node");
    Require(FeedbackReady(owner, prism::contracts::OwnerFeedbackKind::Info) == request.request_id &&
                owner.FocusSnapshot() == before && !owner.Frontend().ActiveOwnerTask(),
            "SDK feedback adoption changed the existing native editor focus");
    report.sdk_focus_feedback_adopted = true;

    // No pointer click or focus setter follows Show: keyboard input must still
    // target the exact editor node and seat captured before the SDK request.
    input.SelectAll();
    for (const auto character : std::string_view("new.md.md")) {
        input.Type(character);
        owner.Pump();
    }
    Wait(
        owner, [&owner] { return owner.String("text_1") == "new.md.md"; },
        "Native keyboard input after SDK feedback required refocusing the editor");
    Require(owner.FocusSnapshot() == before && Focused(owner, "edit:1") &&
                Read(directory / "new.md") == "new" &&
                owner.String("details").find("Unsaved") != std::string::npos,
            "SDK feedback lost the original native focus, draft or saved snapshot");
    report.sdk_existing_focus_preserved = report.sdk_keyboard_without_refocus = true;
}

CaseReport Run(const char *socket, const std::filesystem::path &package,
               const std::filesystem::path &source, const std::filesystem::path &directory,
               Input &input, std::string name, std::uint64_t instance)
{
    Owner owner(socket, package, source, instance);
    Started(owner);
    CaseReport report;
    report.name = name;
    report.native_pointer = report.native_keyboard = true;

    if (name == "open-save-as") {
        input.Click(ActionPoint(owner, "open-panel"));
        FileReady(owner);
        ++report.adopted_tasks;
        SelectFile(owner, input, "existing.md");
        Wait(
            owner, [&owner] { return owner.String("text_1") == "Existing document.\n"; },
            "Real Notepad open did not read the selected file");
        Restored(owner);
        input.Click(ActionPoint(owner, "open-panel"));
        FileReady(owner);
        ++report.adopted_tasks;
        SelectFile(owner, input, "existing.md");
        Restored(owner);
        Require(!owner.Boolean("used_2"), "Opening an existing tab created a duplicate");

        Type(owner, input, "edit:1", "new");
        Wait(
            owner, [&owner] { return owner.String("text_1") == "new"; },
            "Native Notepad text was not accepted by its real module");
        input.Click(ActionPoint(owner, "documents"));
        Wait(
            owner,
            [&owner] {
                const auto input = owner.Adopted();
                return owner.Boolean("documents") && input &&
                       std::none_of(input->nodes.begin(), input->nodes.end(), [](const auto &node) {
                           return node.visible && node.action == "documents";
                       });
            },
            "Notepad overview did not adopt its new visible action layout");
        input.Click(ActionPoint(owner, "save-as"));
        FileReady(owner);
        ++report.adopted_tasks;
        Type(owner, input, prism::runtime::kOwnerFileNameAction, "new.md");
        Wait(
            owner, [&owner] { return owner.host.impl_->owner_task->file->filename == "new.md"; },
            "Native Save As filename did not reach its provider");
        input.Click(ActionPoint(owner, prism::runtime::kOwnerFileSubmitAction));
        Wait(
            owner, [&owner] { return owner.String("status") == "Saved successfully"; },
            "Notepad Save As business work did not finish");
        Restored(owner);
        FeedbackReady(owner, prism::contracts::OwnerFeedbackKind::Success);
        ++report.adopted_feedback;
        Require(!owner.Frontend().ActiveOwnerTask(), "Saved feedback opened a modal task");
        Require(Read(directory / "new.md") == "new" &&
                    Read(directory / "existing.md") == "Existing document.\n",
                "Notepad Save As wrote incorrect contents or modified the old file");
        Type(owner, input, "edit:1", "new.md");
        Wait(
            owner, [&owner] { return owner.String("text_1") == "new.md"; },
            "Saved feedback prevented continuing native text editing");
        FeedbackReady(owner, prism::contracts::OwnerFeedbackKind::Success);
        Require(Focused(owner, "edit:1") && Read(directory / "new.md") == "new" &&
                    owner.String("details").find("Unsaved") != std::string::npos,
                "Editing under Saved feedback lost focus, draft state or saved snapshot");
        report.feedback_continued_editing = true;
        VerifySdkFeedbackFocus(owner, input, directory, report);
        input.Click(ActionPoint(owner, "open-panel"));
        FileReady(owner);
        ++report.adopted_tasks;
        input.Escape();
        Restored(owner);
        Require(owner.String("text_1") == "new.md.md", "Cancelling Open lost the existing draft");
        report.actual_business_io = true;
    } else if (name == "save-overwrite") {
        Type(owner, input, "edit:0", "new");
        Wait(
            owner, [&owner] { return owner.String("text_0") == "new"; },
            "Overwrite case text did not reach Notepad");
        input.Click(ActionPoint(owner, "save"));
        FileReady(owner);
        ++report.adopted_tasks;
        SelectFile(owner, input, "existing.md");
        Wait(
            owner,
            [&owner] {
                return owner.Ready() && owner.host.impl_->owner_task->file->phase ==
                                            prism::sdk::OwnerFileState::Phase::Overwrite;
            },
            "Notepad existing target bypassed provider overwrite confirmation");
        input.Click(ActionPoint(owner, prism::runtime::kOwnerFileReplaceAction));
        Wait(
            owner, [&owner] { return owner.String("status") == "Saved successfully"; },
            "Notepad approved overwrite did not perform real business writing");
        Restored(owner);
        Require(Read(directory / "existing.md") == "new", "Approved overwrite did not save");
        Type(owner, input, "edit:0", "new.md");
        Wait(
            owner, [&owner] { return owner.String("text_0") == "new.md"; },
            "Changed file case did not receive text");
        std::ofstream(directory / "existing.md") << "External changes preserved.\n";
        input.Click(ActionPoint(owner, "save"));
        Wait(
            owner,
            [&owner] {
                return !owner.Boolean("busy") &&
                       owner.String("status").find("changed") != std::string::npos;
            },
            "Notepad did not detect stale on-disk contents");
        Require(owner.String("text_0") == "new.md" &&
                    Read(directory / "existing.md") == "External changes preserved.\n",
                "Failed save destroyed draft or external contents");
        const auto first_error = FeedbackReady(owner, prism::contracts::OwnerFeedbackKind::Error);
        ++report.adopted_feedback;
        const auto &first_feedback = *owner.Frontend().impl_->owner_feedback;
        Require(first_feedback.actions.size() == 1 && first_feedback.actions[0].id == 1 &&
                    first_feedback.actions[0].label == "Change location" &&
                    !first_feedback.duration_ms &&
                    !owner.Frontend().impl_->owner_feedback_session.Deadline(),
                "Save failure offered a blind retry or a timed error");

        // Keep the ordinary Save gate above, then fail the same version check
        // during an actual WM close. Recovery must never resume its retired ID.
        CloseRequest();
        Confirmation(owner);
        ++report.adopted_tasks;
        Require(owner.host.impl_->business->pending_close_.has_value(),
                "Real close never created a deferred business close identity");
        const auto close_id = *owner.host.impl_->business->pending_close_;
        input.Click(ActionPoint(owner, prism::runtime::kOwnerTaskChoiceActions[0]));
        const auto error = FeedbackReady(owner, prism::contracts::OwnerFeedbackKind::Error);
        ++report.adopted_feedback;
        Require(error > first_error && !owner.host.impl_->business->pending_close_ &&
                    !owner.host.impl_->business->close_decision_ &&
                    !owner.host.IsCloseRequested() &&
                    owner.host.impl_->business->next_close_id_ > close_id &&
                    Read(directory / "existing.md") == "External changes preserved.\n" &&
                    owner.String("text_0") == "new.md",
                "Close-save failure did not retire the original close while retaining data");
        input.Click(ActionPoint(owner, FeedbackActionName(owner, 1)));
        FileReady(owner);
        ++report.adopted_tasks;
        ++report.native_feedback_actions;
        Require(owner.host.impl_->owner_task->request.kind ==
                        prism::contracts::OwnerTaskKind::SaveFile &&
                    (!owner.Frontend().impl_->owner_feedback ||
                     owner.Frontend().impl_->owner_feedback->request_id != error),
                "Native recovery did not retire feedback and start a new shared SaveFile task");
        input.Escape();
        Restored(owner);
        Require(owner.String("text_0") == "new.md" &&
                    Read(directory / "existing.md") == "External changes preserved.\n" &&
                    !owner.host.IsCloseRequested() && owner.close_receipts == 1 &&
                    !owner.host.impl_->business->pending_close_ &&
                    !owner.host.impl_->business->close_decision_,
                "Cancelling feedback recovery lost data or revived a close request");
        report.feedback_recovery_retained = report.feedback_close_retired = true;
        report.actual_business_io = true;
    } else if (name == "close-cancel-discard") {
        Type(owner, input, "edit:0", "new");
        Wait(owner, [&owner] { return owner.String("text_0") == "new"; }, "First draft missing");
        input.Click(ActionPoint(owner, "new"));
        Type(owner, input, "edit:1", "new.md");
        Wait(
            owner, [&owner] { return owner.String("text_1") == "new.md"; }, "Second draft missing");
        CloseRequest();
        Confirmation(owner);
        ++report.adopted_tasks;
        const auto first = owner.host.impl_->owner_task->request.request_id;
        CloseRequest();
        Wait(
            owner, [&owner] { return owner.close_receipts == 2; },
            "Second real WM close did not reach the UI callback");
        Require(owner.host.impl_->owner_task->request.request_id == first,
                "Repeated WM close duplicated the active unsaved confirmation");
        input.Click(ActionPoint(owner, prism::runtime::kOwnerTaskChoiceActions[1]));
        Wait(
            owner,
            [&owner, first] {
                return owner.Ready() && owner.host.impl_->owner_task &&
                       owner.host.impl_->owner_task->request.request_id > first;
            },
            "First discard did not continue to the second unsaved document");
        ++report.adopted_tasks;
        input.Click(ActionPoint(owner, prism::runtime::kOwnerTaskCancelAction));
        Restored(owner);
        Require(owner.Boolean("used_0") && owner.Boolean("used_1") &&
                    owner.String("text_0") == "new" && owner.String("text_1") == "new.md" &&
                    !owner.host.IsCloseRequested(),
                "Cancelling sequential close lost a previously approved draft");
        CloseRequest();
        Confirmation(owner);
        ++report.adopted_tasks;
        const auto next = owner.host.impl_->owner_task->request.request_id;
        input.Click(ActionPoint(owner, prism::runtime::kOwnerTaskChoiceActions[1]));
        Wait(
            owner,
            [&owner, next] {
                return owner.Ready() && owner.host.impl_->owner_task &&
                       owner.host.impl_->owner_task->request.request_id > next;
            },
            "Second close did not continue after first discard");
        ++report.adopted_tasks;
        input.Click(ActionPoint(owner, prism::runtime::kOwnerTaskChoiceActions[1]));
        Closed(owner);
        report.real_wm_close = true;
    } else if (name == "close-save") {
        Type(owner, input, "edit:0", "new");
        Wait(
            owner, [&owner] { return owner.String("text_0") == "new"; },
            "Close-save draft missing");
        CloseRequest();
        Confirmation(owner);
        ++report.adopted_tasks;
        input.Click(ActionPoint(owner, prism::runtime::kOwnerTaskChoiceActions[0]));
        FileReady(owner);
        ++report.adopted_tasks;
        Type(owner, input, prism::runtime::kOwnerFileNameAction, "new.md");
        Wait(
            owner, [&owner] { return owner.host.impl_->owner_task->file->filename == "new.md"; },
            "Close-save filename missing");
        input.Click(ActionPoint(owner, prism::runtime::kOwnerFileSubmitAction));
        Closed(owner);
        Require(Read(directory / "new.md") == "new",
                "Async close accepted before the actual business save");
        report.actual_business_io = report.real_wm_close = true;
    } else {
        Require(false, "Unknown Notepad native case");
    }
    report.pixel_commits = owner.Frontend().GetPlatformStatus().surface_pixel_commits;
    report.native_close_events = owner.close_receipts;
    report.feedback_focus_checks = owner.feedback_focus_checks;
    report.feedback_focus_samples = owner.feedback_focus_samples;
    report.feedback_focus_unchanged = owner.feedback_focus_checks && !owner.feedback_focus_changes;
    report.feedback_focus_preserved = owner.feedback_focus_samples && !owner.feedback_focus_changes;
    report.motion = Evidence(owner.motion_adoptions);
    report.passed = true;
    owner.host.Close();
    return report;
}

struct Report {
    bool passed{};
    std::string renderer{"V3D"}, theme{"square/light"}, error;
    std::vector<CaseReport> scenarios;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Report, passed, renderer, theme, error, scenarios)
} // namespace

int main(int argc, char **argv)
{
    if (argc != 5) {
        return 2;
    }
    Report report;
    try {
        Input input;
        input.Connect(argv[1]);
        const auto *home = std::getenv("HOME");
        Require(home && *home, "Notepad native fixture HOME missing");
        std::uint64_t instance = 9100;
        for (const auto *name :
             {"open-save-as", "save-overwrite", "close-cancel-discard", "close-save"}) {
            const std::filesystem::path directory(home);
            std::filesystem::remove(directory / "new.md");
            std::ofstream(directory / "existing.md") << "Existing document.\n";
            report.scenarios.push_back(
                Run(argv[1], argv[2], argv[3], directory, input, name, ++instance));
        }
        report.passed = true;
    } catch (const std::exception &error) {
        report.error = error.what();
        std::cerr << "Notepad native probe failed: " << error.what() << '\n';
    }
    std::ofstream(argv[4]) << nlohmann::json(report).dump(2) << '\n';
    return report.passed ? 0 : 1;
}
