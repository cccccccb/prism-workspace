#include "client_application_p.hpp"
#include "prism/runtime/owner_task_panel.hpp"

#include <limits>

namespace prism::sdk {
namespace {
std::uint64_t NextGeneration(std::uint64_t &last)
{
    if (last == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Task motion generation exhausted");
    }
    return ++last;
}

bool SameValue(const runtime::TaskMotionSample &a, const runtime::TaskMotionSample &b)
{
    return a.reveal == b.reveal && a.endpoint == b.endpoint && a.kind == b.kind;
}
} // namespace

bool ClientApplication::Impl::IsStandardOwnerTask() const
{
    return scene && owner_task_scope && owner_task_scope->ui == installed_ui &&
           owner_task_scope->root == scene->RegionId(runtime::kOwnerTaskPanelRegion) &&
           ((owner_confirmation && owner_confirmation_ui == installed_ui) ||
            (owner_file_view && owner_file_ui == installed_ui));
}

void ClientApplication::Impl::ResetOwnerTaskMotion()
{
    owner_task_motion.reset();
    owner_task_instant.reset();
    last_task_motion_list.reset();
}

void ClientApplication::Impl::FallBackOwnerTaskMotion()
{
    if (!owner_task_motion || owner_task_motion->fallback || !scene) {
        return;
    }

    auto &motion = *owner_task_motion;
    const auto generation = NextGeneration(last_owner_task_motion_generation);
    const auto now = scene->AnimationNowNs();
    const auto state = owner_task_presentation.Current();
    const bool closing = state && state->phase == runtime::TaskPresentationPhase::Closing;
    const bool accepted =
        closing ? motion.timeline.BeginClose({}, 0, now) : motion.timeline.BeginOpen({}, now);
    if (!accepted) {
        throw std::overflow_error("Task motion fallback unavailable");
    }

    motion.spec = {};
    motion.fallback = true;
    motion.adopted.reset();
    motion.current = {motion.identity, generation, 1, motion.timeline.SampleAt(now)};
    owner_task_paint.reset();
    last_task_motion_list.reset();
}

void ClientApplication::Impl::PrepareOwnerTaskMotion(runtime::FramePacket &packet,
                                                     bool paint_available)
{
    const auto state = owner_task_presentation.Current();
    if (!state || state->phase == runtime::TaskPresentationPhase::Closed || !scene ||
        state->identity.ui != installed_ui) {
        ResetOwnerTaskMotion();
        return;
    }
    if ((owner_task_motion && owner_task_motion->identity != state->identity) ||
        (owner_task_instant && *owner_task_instant != state->identity)) {
        ResetOwnerTaskMotion();
    }

    const bool closing = state->phase == runtime::TaskPresentationPhase::Closing;
    if (owner_task_motion && (!closing ? !paint_available : !ClosingOwnerTaskPaint())) {
        FallBackOwnerTaskMotion();
    }
    if (!owner_task_motion && !owner_task_instant && !closing && IsStandardOwnerTask()) {
        const auto spec =
            runtime::ResolveTaskMotionSpec(theme ? theme->motion : contracts::MotionSet{});
        if (paint_available && (spec.opening.duration_ns || spec.closing.duration_ns)) {
            auto motion = std::make_unique<OwnerTaskMotion>(
                state->identity, NextGeneration(last_owner_task_motion_generation),
                scene->AnimationClockSource(), spec);
            auto entrance = spec;
            if (state->phase == runtime::TaskPresentationPhase::Open) {
                entrance.opening = {}; // A refreshed Open projection never replays entrance.
            }
            const auto now = scene->AnimationNowNs();
            if (!motion->timeline.BeginOpen(entrance, now)) {
                throw std::runtime_error("Task opening trajectory unavailable");
            }
            motion->current.sample = motion->timeline.SampleAt(now);
            owner_task_motion = std::move(motion);
        } else {
            // Once this cycle publishes an immediate endpoint, later export
            // support cannot replay entrance or publish Intermediate after it.
            owner_task_instant = state->identity;
        }
    }
    if (owner_task_motion) {
        packet.task_motion = owner_task_motion->current;
    }
}

bool ClientApplication::Impl::AdvanceOwnerTaskMotion(std::uint64_t now)
{
    if (!owner_task_motion || !owner_task_motion->timeline.IsActive()) {
        return false;
    }
    auto &motion = *owner_task_motion;
    const auto sample = motion.timeline.SampleAt(now);
    if (SameValue(sample, motion.current.sample)) {
        return false;
    }
    if (motion.current.revision == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Task motion sample revision exhausted");
    }

    ++motion.current.revision;
    motion.current.sample = sample;
    return true;
}

void ClientApplication::Impl::BeginClosingOwnerTaskMotion()
{
    const auto state = owner_task_presentation.Current();
    if (!owner_task_motion || !state || state->phase != runtime::TaskPresentationPhase::Closing ||
        owner_task_motion->identity != state->identity || !scene) {
        ResetOwnerTaskMotion();
        return;
    }
    if (!ClosingOwnerTaskPaint() || !owner_task_motion->adopted) {
        FallBackOwnerTaskMotion();
    }

    auto &motion = *owner_task_motion;
    const auto generation = NextGeneration(last_owner_task_motion_generation);
    const auto from = motion.adopted ? motion.adopted->sample.reveal : 0;
    const auto now = scene->AnimationNowNs();
    if (!motion.timeline.BeginClose(motion.spec, from, now)) {
        throw std::overflow_error("Task closing trajectory unavailable");
    }
    motion.current = {motion.identity, generation, 1, motion.timeline.SampleAt(now)};
    motion.adopted.reset();
    last_task_motion_list.reset();
}

bool ClientApplication::Impl::HasActiveAnimations() const noexcept
{
    return scene && (scene->HasActiveAnimations() ||
                     (owner_task_motion && owner_task_motion->timeline.IsActive()));
}

bool ClientApplication::Impl::AdvanceAnimations(std::uint64_t now)
{
    const bool scene_changed = scene && scene->AdvanceAnimations(now);
    const bool task_changed = AdvanceOwnerTaskMotion(now);
    return scene_changed || task_changed;
}

std::optional<std::uint64_t>
ClientApplication::Impl::NextAnimationDeadlineNs(std::uint64_t now) const noexcept
{
    auto deadline = scene ? scene->NextAnimationDeadlineNs(now) : std::nullopt;
    if (owner_task_motion) {
        const auto task_deadline = owner_task_motion->timeline.CompletionDeadlineNs();
        if (task_deadline && (!deadline || *task_deadline < *deadline)) {
            deadline = task_deadline;
        }
    }
    return deadline;
}

} // namespace prism::sdk
