#include "prism/runtime/task_presentation.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr TaskIdentity task{{3}, {1}};
constexpr TaskIdentity next_task{{3}, {2}};
constexpr UiLoadId ui{5, 7};
constexpr contracts::NodeId root{4, 2};
constexpr std::uint64_t token = 11;
constexpr std::uint64_t epoch = 12;

// Pure typed protocol tests: Adopt models an independently verified adapter
// receipt, not native input, worker adoption, pixel submission or presentation.
// Private access only observes atomic rejection and seeds unreachable counter
// limits; production exposes no test hook and uses no synthetic receipt source.
struct Observation {
    std::optional<TaskPresentationState> state;
    std::uint64_t cycle{}, frame_sequence{};
    std::uint64_t scope_token{}, scope_epoch{};
    contracts::NodeId scope_root;
    bool ever_adopted{};
    std::uint64_t first_published{}, last_published{}, first_terminal{};

    bool operator==(const Observation &) const noexcept = default;
};

Observation Observe(const TaskPresentationSession &session)
{
    return {session.Current(),
            session.last_cycle_,
            session.last_frame_sequence_,
            session.scope_.token,
            session.scope_.epoch,
            session.scope_.root,
            session.ever_adopted_,
            session.first_published_sequence_,
            session.last_published_sequence_,
            session.first_terminal_sequence_};
}

TaskPresentationBinding Binding()
{
    return {token, epoch, root, 27, 2, 4, 640, 360, 1.0, 1};
}

struct Fixture {
    TaskPresentationSession session;
    TaskPresentationIdentity identity;

    Fixture()
    {
        const auto begun = session.Begin(task, ui, token, epoch, root);
        assert(begun);
        identity = *begun;
    }

    TaskPresentationStamp Publish(const TaskPresentationBinding &binding, std::uint64_t sequence)
    {
        const auto stamp = session.Publish(identity, binding, sequence);
        assert(stamp);
        return *stamp;
    }

    TaskPresentationStamp Open()
    {
        const auto stamp = Publish(Binding(), 1);
        assert(session.Adopt(stamp));
        assert(session.Current()->phase == TaskPresentationPhase::Open);
        return stamp;
    }
};

TaskPresentationBinding ClosedBinding(std::uint64_t closed_epoch = epoch + 1)
{
    auto binding = Binding();
    binding.modal_token = 0;
    binding.modal_epoch = closed_epoch;
    binding.root = {};
    ++binding.input_version;
    return binding;
}

void CheckEmptyAndBeginValidation()
{
    static_assert(!std::is_copy_constructible_v<TaskPresentationSession>);
    static_assert(!std::is_move_constructible_v<TaskPresentationSession>);
    TaskPresentationSession session;
    assert(!session.Current());
    const auto before = Observe(session);
    assert(!session.Begin({{}, task.request}, ui, token, epoch, root));
    assert(!session.Begin({task.owner, {}}, ui, token, epoch, root));
    assert(!session.Begin(task, {0, ui.generation}, token, epoch, root));
    assert(!session.Begin(task, {ui.owner, 0}, token, epoch, root));
    assert(!session.Begin(task, ui, 0, epoch, root));
    assert(!session.Begin(task, ui, token, 0, root));
    assert(!session.Begin(task, ui, token, epoch, {}));
    assert(!session.Begin(task, ui, token, epoch, {root.index, 0}));
    assert(!session.Begin(task, ui, token, epoch, {UINT32_MAX, root.generation}));
    assert(Observe(session) == before);

    const auto identity = session.Begin(task, ui, token, epoch, root);
    assert(identity && identity->task == task && identity->ui == ui && identity->cycle == 1);
    const auto state = session.Current();
    assert(state && state->identity == *identity && state->phase == TaskPresentationPhase::Opening);
    assert(state->projection == 1 && !state->binding && !state->adopted_sequence &&
           !state->interruption);
    const auto opening = Observe(session);
    assert(!session.Begin(next_task, ui, token + 1, epoch + 1, root));
    assert(Observe(session) == opening);

    auto foreign = *identity;
    ++foreign.cycle;
    assert(!session.Publish(foreign, Binding(), 1));
    assert(!session.InvalidateProjection(foreign));
    assert(!session.Reproject(foreign, token + 1, epoch + 1, root));
    assert(!session.BeginClose(foreign, epoch + 1));
    assert(!session.Interrupt(foreign, TaskPresentationInterruptReason::Superseded));
    assert(Observe(session) == opening);
}

void CheckPublicationAndBusinessIndependence()
{
    TaskSession business(task.owner);
    const auto request = business.Begin();
    assert(request && *request == task);
    Fixture f;
    const auto stamp = f.Publish(Binding(), 1);
    assert(stamp.identity == f.identity && stamp.projection == 1 &&
           stamp.endpoint == TaskPresentationEndpoint::Open && stamp.binding == Binding());
    assert(f.session.Current()->phase == TaskPresentationPhase::Opening);
    assert(!f.session.Current()->adopted_sequence);
    assert(business.Active()->phase == TaskPhase::Preparing);

    assert(f.session.Adopt(stamp));
    assert(f.session.Current()->phase == TaskPresentationPhase::Open);
    assert(f.session.Current()->adopted_sequence == 1);
    assert(business.Active()->phase == TaskPhase::Preparing); // Presentation grants no TaskReady.
    // Model the separately authenticated input-adoption gate, not a timer callback.
    assert(business.SetReady(*request));
    assert(business.Succeed(*request));
    assert(f.session.Current()->phase == TaskPresentationPhase::Open);

    assert(f.session.BeginClose(f.identity, epoch + 1));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closing);
    const auto terminal = business.TakeTerminal();
    assert(terminal && terminal->outcome == TaskOutcome::Success);
    assert(!business.Active()); // Business delivery does not wait for visual close adoption.
    const auto closed = f.Publish(ClosedBinding(), 2);
    assert(closed.endpoint == TaskPresentationEndpoint::Closed);
    assert(f.session.Current()->phase == TaskPresentationPhase::Closing);
    assert(f.session.Adopt(closed));
    const auto state = f.session.Current();
    assert(state->phase == TaskPresentationPhase::Closed && state->adopted_sequence == 2 &&
           state->binding == ClosedBinding() && !state->interruption);
    assert(!business.TakeTerminal()); // Presentation cannot generate another task result.
}

void CheckStampValidationAndPublicationRange()
{
    Fixture f;
    const auto earlier = f.Publish(Binding(), 10);
    const auto latest = f.Publish(Binding(), 20);
    assert(earlier.projection == latest.projection);
    const auto before = Observe(f.session);
    auto stamp = latest;
    ++stamp.identity.task.owner.value;
    assert(!f.session.Adopt(stamp));
    stamp = latest;
    ++stamp.identity.task.request.value;
    assert(!f.session.Adopt(stamp));
    stamp = latest;
    ++stamp.identity.ui.owner;
    assert(!f.session.Adopt(stamp));
    stamp = latest;
    ++stamp.identity.ui.generation;
    assert(!f.session.Adopt(stamp));
    stamp = latest;
    ++stamp.identity.cycle;
    assert(!f.session.Adopt(stamp));
    stamp = latest;
    stamp.projection = 0;
    assert(!f.session.Adopt(stamp));
    stamp = latest;
    ++stamp.projection;
    assert(!f.session.Adopt(stamp));
    stamp = latest;
    stamp.endpoint = TaskPresentationEndpoint::Closed;
    assert(!f.session.Adopt(stamp));
    stamp = latest;
    stamp.endpoint = static_cast<TaskPresentationEndpoint>(99);
    assert(!f.session.Adopt(stamp));
    stamp = latest;
    ++stamp.binding.input_version;
    assert(!f.session.Adopt(stamp));
    for (const auto sequence : std::array<std::uint64_t, 4>{0, 9, 21, UINT64_MAX}) {
        stamp = latest;
        stamp.frame_sequence = sequence;
        assert(!f.session.Adopt(stamp));
    }
    assert(Observe(f.session) == before);

    assert(f.session.Adopt(earlier)); // A valid older publication need not be the newest packet.
    assert(f.session.Current()->adopted_sequence == 10);
    assert(f.session.Adopt(latest));
    const auto adopted = Observe(f.session);
    assert(!f.session.Adopt(latest));
    assert(!f.session.Adopt(earlier));
    assert(Observe(f.session) == adopted);
    const auto next = f.Publish(Binding(), 21);
    assert(f.session.Current()->adopted_sequence == 20);
    assert(next.projection == latest.projection && f.session.Adopt(next));
    assert(f.session.Current()->phase == TaskPresentationPhase::Open);
}

void CheckInvalidBindingsRejectAtomically()
{
    Fixture f;
    const auto before = Observe(f.session);
    const auto valid = Binding();
    std::array<TaskPresentationBinding, 17> invalid;
    invalid.fill(valid);
    invalid[0].modal_token = 0;
    invalid[1].modal_epoch = 0;
    invalid[2].root = {};
    ++invalid[3].modal_token;
    ++invalid[4].modal_epoch;
    ++invalid[5].root.generation;
    invalid[6].input_scene = 0;
    invalid[7].input_version = 0;
    invalid[8].configure_count = 0;
    invalid[9].configure_count = -1;
    invalid[10].buffer_width = 0;
    invalid[11].buffer_height = 0;
    invalid[12].scale = 0;
    invalid[13].scale = -1;
    invalid[14].scale = std::numeric_limits<double>::infinity();
    invalid[15].scale = -std::numeric_limits<double>::infinity();
    invalid[16].scale = std::numeric_limits<double>::quiet_NaN();
    for (const auto &binding : invalid) {
        assert(!f.session.Publish(f.identity, binding, 1));
        assert(Observe(f.session) == before);
    }
    assert(!f.session.Publish(f.identity, valid, 0));
    assert(Observe(f.session) == before);

    auto no_theme = valid;
    no_theme.theme_generation = 0;
    const auto stamp = f.Publish(no_theme, 1); // No theme snapshot is a valid frontend state.
    assert(f.session.Adopt(stamp));
    const auto adopted = Observe(f.session);
    assert(!f.session.Publish(f.identity, no_theme, 1));
    assert(!f.session.Publish(f.identity, no_theme, 0));
    assert(Observe(f.session) == adopted);
}

void CheckRefreshPreservesCycleAndOpenPhase()
{
    Fixture f;
    const auto original = f.Publish(Binding(), 1);
    const auto before = Observe(f.session);
    assert(!f.session.Reproject(f.identity, token, epoch, root));
    assert(!f.session.Reproject(f.identity, token, epoch - 1, root));
    assert(!f.session.Reproject(f.identity, 0, epoch + 1, root));
    assert(!f.session.Reproject(f.identity, token, epoch + 1, {}));
    assert(Observe(f.session) == before);

    constexpr contracts::NodeId refreshed_root{7, 3};
    assert(f.session.Reproject(f.identity, 21, 22, refreshed_root));
    auto state = f.session.Current();
    assert(state->identity == f.identity && state->phase == TaskPresentationPhase::Opening &&
           state->projection == original.projection + 1 && !state->binding &&
           !state->adopted_sequence);
    assert(!f.session.Adopt(original));
    auto binding = Binding();
    binding.modal_token = 21;
    binding.modal_epoch = 22;
    binding.root = refreshed_root;
    ++binding.input_version;
    const auto refreshed = f.Publish(binding, 2);
    assert(f.session.Adopt(refreshed));

    assert(f.session.Reproject(f.identity, 31, 32, refreshed_root));
    state = f.session.Current();
    assert(state->identity == f.identity && state->phase == TaskPresentationPhase::Open &&
           state->projection == refreshed.projection + 1 && !state->binding &&
           !state->adopted_sequence);
    assert(!f.session.Adopt(refreshed));
    assert(f.session.ever_adopted_); // Refresh never replays an already adopted entrance.
    assert(f.session.BeginClose(f.identity, 33));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closing);
    auto closed = ClosedBinding(33);
    ++closed.input_version;
    const auto stamp = f.Publish(closed, 3);
    assert(f.session.Adopt(stamp));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closed);
}

void CheckInvalidationAndDescriptorChanges()
{
    Fixture f;
    auto stamp = f.Open();
    assert(f.session.InvalidateProjection(f.identity));
    auto state = f.session.Current();
    assert(state->identity == f.identity && state->phase == TaskPresentationPhase::Open &&
           state->projection == stamp.projection + 1 && !state->binding &&
           !state->adopted_sequence);
    assert(!f.session.Adopt(stamp));
    auto binding = Binding();
    stamp = f.Publish(binding, 2);
    assert(f.session.Adopt(stamp));

    for (std::size_t field = 0; field != 7; ++field) {
        auto next = binding;
        switch (field) {
        case 0:
            ++next.input_scene;
            break;
        case 1:
            ++next.input_version;
            break;
        case 2:
            ++next.configure_count;
            break;
        case 3:
            ++next.buffer_width;
            break;
        case 4:
            ++next.buffer_height;
            break;
        case 5:
            next.scale = 2;
            break;
        case 6:
            ++next.theme_generation;
            break;
        }
        const auto previous = stamp;
        stamp = f.Publish(next, field + 3);
        state = f.session.Current();
        assert(state->phase == TaskPresentationPhase::Open && state->identity == f.identity &&
               state->projection == previous.projection + 1 && !state->adopted_sequence);
        assert(!f.session.Adopt(previous));
        assert(f.session.Adopt(stamp));
        binding = next;
    }
    const auto latest = f.Publish(binding, 10);
    assert(latest.projection == stamp.projection);
    assert(f.session.Current()->adopted_sequence == stamp.frame_sequence);
    assert(f.session.Adopt(latest));
}

void CheckFastCancellationWithoutAdoptedEntrance()
{
    Fixture f;
    const auto pending = f.Publish(Binding(), 1);
    assert(f.session.BeginClose(f.identity, epoch + 1));
    const auto closed = f.session.Current();
    assert(closed->phase == TaskPresentationPhase::Closed && !closed->binding &&
           !closed->adopted_sequence && !closed->interruption);
    assert(!f.session.Adopt(pending));
    assert(!f.session.Publish(f.identity, ClosedBinding(), 2));
    assert(!f.session.BeginClose(f.identity, epoch + 2));

    const auto next = f.session.Begin(next_task, ui, 14, 14, root);
    assert(next && next->cycle == f.identity.cycle + 1);
    const auto before = Observe(f.session);
    assert(!f.session.BeginClose(f.identity, epoch + 2));
    assert(!f.session.Interrupt(f.identity, TaskPresentationInterruptReason::Superseded));
    assert(Observe(f.session) == before);
    auto binding = Binding();
    binding.modal_token = 14;
    binding.modal_epoch = 14;
    const auto stamp = f.session.Publish(*next, binding, 2);
    assert(stamp && f.session.Adopt(*stamp));
}

void CheckCloseRequiresRetiredScopeAndAdoption()
{
    Fixture f;
    const auto opened = f.Open();
    const auto before = Observe(f.session);
    assert(!f.session.BeginClose(f.identity, 0));
    assert(!f.session.BeginClose(f.identity, epoch));
    assert(!f.session.BeginClose(f.identity, epoch - 1));
    assert(Observe(f.session) == before);
    assert(f.session.BeginClose(f.identity, epoch + 1));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closing);
    assert(!f.session.Current()->binding && !f.session.Current()->adopted_sequence);
    assert(!f.session.Adopt(opened));
    const auto closing = Observe(f.session);
    assert(!f.session.BeginClose(f.identity, epoch + 2));
    assert(!f.session.Reproject(f.identity, 14, 14, root));
    assert(!f.session.InvalidateProjection(f.identity));
    assert(!f.session.Publish(f.identity, Binding(), 2));
    assert(Observe(f.session) == closing);

    auto binding = ClosedBinding();
    binding.root = {root.index, 0}; // A malformed identity is not the retired root descriptor.
    assert(!f.session.Publish(f.identity, binding, 2));
    binding = ClosedBinding();
    binding.modal_token = token;
    assert(!f.session.Publish(f.identity, binding, 2));
    binding = ClosedBinding();
    ++binding.modal_epoch;
    assert(!f.session.Publish(f.identity, binding, 2));
    assert(Observe(f.session) == closing);

    const auto closed = f.Publish(ClosedBinding(), 2);
    assert(closed.endpoint == TaskPresentationEndpoint::Closed);
    auto wrong_endpoint = closed;
    wrong_endpoint.endpoint = TaskPresentationEndpoint::Open;
    const auto published = Observe(f.session);
    assert(!f.session.Adopt(wrong_endpoint));
    assert(Observe(f.session) == published);
    assert(f.session.Adopt(closed));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closed);
    const auto adopted = Observe(f.session);
    assert(!f.session.Adopt(closed));
    assert(!f.session.BeginClose(f.identity, epoch + 2));
    assert(!f.session.Interrupt(f.identity, TaskPresentationInterruptReason::UiReplaced));
    assert(Observe(f.session) == adopted);
}

void CheckClosingSupersessionAndOldCloseIsolation()
{
    Fixture f;
    const auto opened = f.Open();
    assert(f.session.BeginClose(f.identity, epoch + 1));
    const auto closing = f.Publish(ClosedBinding(), 2);
    const auto next = f.session.Begin(next_task, ui, 14, 14, root);
    assert(next && next->cycle > f.identity.cycle);
    assert(f.session.Current()->phase == TaskPresentationPhase::Opening);
    const auto before = Observe(f.session);
    assert(!f.session.Adopt(opened));
    assert(!f.session.Adopt(closing));
    assert(!f.session.BeginClose(f.identity, epoch + 2));
    assert(!f.session.Interrupt(f.identity, TaskPresentationInterruptReason::Superseded));
    assert(Observe(f.session) == before);

    auto binding = Binding();
    binding.modal_token = 14;
    binding.modal_epoch = 14;
    assert(!f.session.Publish(*next, binding, 2)); // Frame order spans cycles.
    const auto new_stamp = f.session.Publish(*next, binding, 3);
    assert(new_stamp && f.session.Adopt(*new_stamp));
    assert(f.session.Current()->identity == *next &&
           f.session.Current()->phase == TaskPresentationPhase::Open);
    assert(!f.session.Begin(next_task, ui, 15, 15, root));
}

void CheckInterruptionIsOnceAndExact()
{
    constexpr std::array reasons{TaskPresentationInterruptReason::UiReplaced,
                                 TaskPresentationInterruptReason::OwnerRetired,
                                 TaskPresentationInterruptReason::FrontendFailed,
                                 TaskPresentationInterruptReason::GeometryChanged,
                                 TaskPresentationInterruptReason::ThemeChanged,
                                 TaskPresentationInterruptReason::ScopeUnavailable,
                                 TaskPresentationInterruptReason::Superseded};
    for (const auto reason : reasons) {
        for (int phase = 0; phase != 3; ++phase) {
            Fixture f;
            auto stamp = f.Publish(Binding(), 1);
            if (phase > 0) {
                assert(f.session.Adopt(stamp));
            }
            if (phase == 2) {
                assert(f.session.BeginClose(f.identity, epoch + 1));
                stamp = f.Publish(ClosedBinding(), 2);
            }
            const auto before = Observe(f.session);
            assert(
                !f.session.Interrupt(f.identity, static_cast<TaskPresentationInterruptReason>(99)));
            assert(Observe(f.session) == before);
            assert(f.session.Interrupt(f.identity, reason));
            const auto state = f.session.Current();
            assert(state->phase == TaskPresentationPhase::Closed && state->interruption == reason &&
                   !state->binding && !state->adopted_sequence);
            const auto interrupted = Observe(f.session);
            assert(!f.session.Adopt(stamp));
            assert(!f.session.Interrupt(f.identity, TaskPresentationInterruptReason::UiReplaced));
            assert(Observe(f.session) == interrupted);
            const auto next = f.session.Begin(next_task, ui, token + 1, epoch + 1, root);
            assert(next && next->cycle > f.identity.cycle && !f.session.Current()->interruption);
        }
    }
}

void CheckIntermediateOpeningAndTerminalFrontier()
{
    Fixture f;
    const auto first =
        f.session.Publish(f.identity, Binding(), 10, TaskPresentationSampleKind::Intermediate);
    const auto middle =
        f.session.Publish(f.identity, Binding(), 20, TaskPresentationSampleKind::Intermediate);
    assert(first && middle && first->endpoint == TaskPresentationEndpoint::Open);
    assert(f.session.Adopt(*first));
    assert(f.session.Current()->phase == TaskPresentationPhase::Opening);
    assert(f.session.ever_adopted_ && f.session.Current()->adopted_sequence == 10);

    const auto terminal = f.Publish(Binding(), 30);
    const auto before = Observe(f.session);
    assert(!f.session.Publish(f.identity, Binding(), 40, TaskPresentationSampleKind::Intermediate));
    assert(
        !f.session.Publish(f.identity, Binding(), 40, static_cast<TaskPresentationSampleKind>(99)));
    auto forged = *middle;
    forged.sample_kind = TaskPresentationSampleKind::Terminal;
    assert(!f.session.Adopt(forged));
    forged = terminal;
    forged.sample_kind = TaskPresentationSampleKind::Intermediate;
    assert(!f.session.Adopt(forged));
    forged.sample_kind = static_cast<TaskPresentationSampleKind>(99);
    assert(!f.session.Adopt(forged));
    assert(Observe(f.session) == before);

    // Publishing a terminal does not make an older consumed intermediate terminal.
    assert(f.session.Adopt(*middle));
    assert(f.session.Current()->phase == TaskPresentationPhase::Opening);
    assert(f.session.Adopt(terminal));
    assert(f.session.Current()->phase == TaskPresentationPhase::Open);
    auto changed = Binding();
    ++changed.input_version;
    const auto opened = Observe(f.session);
    assert(!f.session.Publish(f.identity, changed, 40, TaskPresentationSampleKind::Intermediate));
    assert(Observe(f.session) == opened);
    assert(f.session.Adopt(f.Publish(changed, 40)));
    assert(f.session.Current()->phase == TaskPresentationPhase::Open);
}

void CheckHalfwayCancellationAndIntermediateClosing()
{
    TaskSession business(task.owner);
    const auto request = business.Begin();
    assert(request);
    Fixture f;
    const auto opening =
        f.session.Publish(f.identity, Binding(), 1, TaskPresentationSampleKind::Intermediate);
    assert(opening && f.session.Adopt(*opening));
    assert(f.session.Current()->phase == TaskPresentationPhase::Opening);
    assert(business.Active()->phase == TaskPhase::Preparing);
    assert(business.SetReady(*request)); // Actual input readiness remains a separate gate.
    assert(business.Cancel(*request, TaskCancelReason::User));
    assert(f.session.BeginClose(f.identity, epoch + 1));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closing);
    assert(business.TakeTerminal() && !business.Active());
    assert(!f.session.Adopt(*opening));

    const auto closing =
        f.session.Publish(f.identity, ClosedBinding(), 2, TaskPresentationSampleKind::Intermediate);
    assert(closing && closing->endpoint == TaskPresentationEndpoint::Closed);
    assert(f.session.Adopt(*closing));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closing);
    assert(f.session.Current()->adopted_sequence == 2);
    const auto terminal = f.Publish(ClosedBinding(), 3);
    auto forged = *closing;
    forged.sample_kind = TaskPresentationSampleKind::Terminal;
    assert(!f.session.Adopt(forged));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closing);
    assert(f.session.Adopt(terminal));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closed);
    assert(!business.TakeTerminal());
}

void CheckUnadoptedIntermediateDoesNotCreateExit()
{
    Fixture f;
    const auto opening =
        f.session.Publish(f.identity, Binding(), 1, TaskPresentationSampleKind::Intermediate);
    assert(opening);
    assert(f.session.BeginClose(f.identity, epoch + 1));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closed);
    assert(!f.session.Adopt(*opening));
    assert(!f.session.Publish(f.identity, ClosedBinding(), 2,
                              TaskPresentationSampleKind::Intermediate));
}

void CheckRefreshResetsOnlyTheProjectionFrontier()
{
    Fixture f;
    const auto old_terminal = f.Publish(Binding(), 1);
    assert(f.session.Reproject(f.identity, token + 1, epoch + 1, root));
    auto refreshed = Binding();
    ++refreshed.modal_token;
    ++refreshed.modal_epoch;
    ++refreshed.input_version;
    const auto middle =
        f.session.Publish(f.identity, refreshed, 2, TaskPresentationSampleKind::Intermediate);
    assert(middle && middle->projection > old_terminal.projection);
    assert(!f.session.Adopt(old_terminal));
    assert(f.session.Adopt(*middle));
    assert(f.session.Current()->identity == f.identity);
    assert(f.session.Current()->phase == TaskPresentationPhase::Opening);

    const auto terminal = f.Publish(refreshed, 3);
    assert(f.session.InvalidateProjection(f.identity));
    assert(!f.session.Adopt(terminal));
    const auto next_middle =
        f.session.Publish(f.identity, refreshed, 4, TaskPresentationSampleKind::Intermediate);
    assert(next_middle && f.session.Adopt(*next_middle));
    assert(f.session.BeginClose(f.identity, epoch + 2));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closing);
}

void CheckChangedBindingCreatesNewSampleFrontier()
{
    for (const bool closing : {false, true}) {
        Fixture f;
        if (closing) {
            f.Open();
            assert(f.session.BeginClose(f.identity, epoch + 1));
        }
        auto binding = closing ? ClosedBinding() : Binding();
        const auto old_terminal = f.Publish(binding, 10);
        ++binding.input_version;
        const auto middle =
            f.session.Publish(f.identity, binding, 20, TaskPresentationSampleKind::Intermediate);
        assert(middle && middle->projection > old_terminal.projection);
        assert(!f.session.Adopt(old_terminal));
        assert(f.session.Adopt(*middle));
        assert(f.session.Current()->phase ==
               (closing ? TaskPresentationPhase::Closing : TaskPresentationPhase::Opening));
        assert(f.session.Adopt(f.Publish(binding, 30)));
        assert(f.session.Current()->phase ==
               (closing ? TaskPresentationPhase::Closed : TaskPresentationPhase::Open));
    }
}

void CheckIntermediateSupersessionAndSequenceExhaustion()
{
    Fixture f;
    const auto opening =
        f.session.Publish(f.identity, Binding(), 1, TaskPresentationSampleKind::Intermediate);
    assert(opening && f.session.Adopt(*opening));
    assert(f.session.BeginClose(f.identity, epoch + 1));
    const auto closing =
        f.session.Publish(f.identity, ClosedBinding(), 2, TaskPresentationSampleKind::Intermediate);
    const auto terminal = f.Publish(ClosedBinding(), 3);
    const auto next = f.session.Begin(next_task, ui, token + 2, epoch + 2, root);
    assert(next && closing);
    const auto begun = Observe(f.session);
    assert(!f.session.Adopt(*closing));
    assert(!f.session.Adopt(terminal));
    assert(Observe(f.session) == begun);

    auto binding = Binding();
    binding.modal_token += 2;
    binding.modal_epoch += 2;
    const auto last =
        f.session.Publish(*next, binding, UINT64_MAX, TaskPresentationSampleKind::Intermediate);
    assert(last && f.session.Adopt(*last));
    assert(f.session.Current()->phase == TaskPresentationPhase::Opening);
    const auto exhausted = Observe(f.session);
    assert(!f.session.Publish(*next, binding, UINT64_MAX));
    assert(Observe(f.session) == exhausted);
    assert(f.session.Interrupt(*next, TaskPresentationInterruptReason::FrontendFailed));
    assert(f.session.Current()->phase == TaskPresentationPhase::Closed);
}

void CheckCounterExhaustionAndSafeCleanup()
{
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    TaskPresentationSession exhausted_cycle;
    exhausted_cycle.last_cycle_ = maximum;
    const auto empty = Observe(exhausted_cycle);
    assert(!exhausted_cycle.Begin(task, ui, token, epoch, root));
    assert(Observe(exhausted_cycle) == empty);

    Fixture cycle;
    assert(cycle.session.Interrupt(cycle.identity, TaskPresentationInterruptReason::Superseded));
    cycle.session.last_cycle_ = maximum;
    const auto retired = Observe(cycle.session);
    assert(!cycle.session.Begin(next_task, ui, token + 1, epoch + 1, root));
    assert(Observe(cycle.session) == retired);

    Fixture projection;
    projection.Open();
    projection.session.current_->projection = maximum;
    const auto same = projection.Publish(Binding(), 2);
    assert(same.projection == maximum && projection.session.Adopt(same));
    const auto before = Observe(projection.session);
    assert(!projection.session.InvalidateProjection(projection.identity));
    assert(!projection.session.Reproject(projection.identity, token + 1, epoch + 1, root));
    assert(!projection.session.BeginClose(projection.identity, epoch + 1));
    auto changed = Binding();
    ++changed.input_version;
    assert(!projection.session.Publish(projection.identity, changed, 3));
    assert(Observe(projection.session) == before);
    // The adapter already retired task input before BeginClose. Its mandatory
    // fallback can still withdraw this cycle without incrementing projection.
    assert(projection.session.Interrupt(projection.identity,
                                        TaskPresentationInterruptReason::ScopeUnavailable));
    assert(projection.session.Current()->phase == TaskPresentationPhase::Closed);
    assert(!projection.session.Adopt(same));

    Fixture sequence;
    sequence.Open();
    const auto final = sequence.Publish(Binding(), maximum);
    assert(final.frame_sequence == maximum && sequence.session.Adopt(final));
    const auto limit = Observe(sequence.session);
    assert(!sequence.session.Publish(sequence.identity, Binding(), maximum));
    assert(!sequence.session.Publish(sequence.identity, Binding(), 1));
    assert(!sequence.session.Publish(sequence.identity, Binding(), 0));
    assert(Observe(sequence.session) == limit);
    assert(sequence.session.Interrupt(sequence.identity,
                                      TaskPresentationInterruptReason::FrontendFailed));
}
} // namespace

int main()
{
    CheckEmptyAndBeginValidation();
    CheckPublicationAndBusinessIndependence();
    CheckStampValidationAndPublicationRange();
    CheckInvalidBindingsRejectAtomically();
    CheckRefreshPreservesCycleAndOpenPhase();
    CheckInvalidationAndDescriptorChanges();
    CheckFastCancellationWithoutAdoptedEntrance();
    CheckCloseRequiresRetiredScopeAndAdoption();
    CheckClosingSupersessionAndOldCloseIsolation();
    CheckInterruptionIsOnceAndExact();
    CheckIntermediateOpeningAndTerminalFrontier();
    CheckHalfwayCancellationAndIntermediateClosing();
    CheckUnadoptedIntermediateDoesNotCreateExit();
    CheckRefreshResetsOnlyTheProjectionFrontier();
    CheckChangedBindingCreatesNewSampleFrontier();
    CheckIntermediateSupersessionAndSequenceExhaustion();
    CheckCounterExhaustionAndSafeCleanup();
}
