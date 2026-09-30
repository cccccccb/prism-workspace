#include "prism/runtime/input_snapshot.hpp"
#include "prism/runtime/pollable_queue.hpp"
#include "prism/runtime/render_command.hpp"
#include "prism/runtime/render_event.hpp"
#include <cassert>
#include <memory>
#include <variant>

namespace {
using prism::runtime::AnswerFrameOpportunityCommand;
using prism::runtime::FrameCommand;
using prism::runtime::FrameOpportunityEvent;
using prism::runtime::InstallUiCommand;
using prism::runtime::InvalidateFrameCommand;
using prism::runtime::PollableQueue;
using prism::runtime::QueuePushResult;
using prism::runtime::RegisterImageCommand;
using prism::runtime::ReleaseImageCommand;
using prism::runtime::RenderCommand;
using prism::runtime::RenderEvent;
using prism::runtime::RenderRequestKind;
using prism::runtime::RenderStatusEvent;
using prism::runtime::RequestRenderCommand;
using prism::runtime::SequencedWindowEvent;
using prism::runtime::SetAnimationSamplingCommand;
using prism::runtime::UiEventsProcessedCommand;

std::shared_ptr<const prism::runtime::FramePacket> Frame(std::uint64_t ui, std::uint64_t revision)
{
    auto frame = std::make_shared<prism::runtime::FramePacket>();
    frame->ui = {1, ui};
    frame->sequence = revision;
    frame->scene_revision = revision;
    return frame;
}

std::shared_ptr<const prism::runtime::InputSnapshot> InputGeometry(std::uint64_t version,
                                                                   std::uint64_t scene = 1)
{
    auto snapshot = std::make_shared<prism::runtime::InputSnapshot>();
    snapshot->scene = scene;
    snapshot->version = version;
    return snapshot;
}

void VerifyFrameReplacement()
{
    PollableQueue<RenderCommand> queue(3);
    RenderCommand first(FrameCommand{Frame(1, 1)});
    RenderCommand latest(FrameCommand{Frame(1, 2)});
    RenderCommand master(FrameCommand{Frame(2, 1)});

    assert(queue.TryPushLatest(std::move(first), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(latest), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Replaced);
    assert(queue.TryPushLatest(std::move(master), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);

    auto preview = queue.TryPop();
    auto next = queue.TryPop();
    assert(preview && next && !queue.TryPop());
    assert(std::get<FrameCommand>(*preview).frame->scene_revision == 2);
    const prism::runtime::UiLoadId master_id{1, 2};
    assert(std::get<FrameCommand>(*next).frame->ui == master_id);
}

void VerifyResourceBarriersAndLease()
{
    PollableQueue<RenderCommand> queue(3);
    const prism::runtime::ImageVersion version{{7}, 12};
    auto pixels = std::make_shared<const prism::runtime::DecodedImage>();
    std::weak_ptr<const prism::runtime::DecodedImage> retained = pixels;
    RenderCommand registration(RegisterImageCommand{version, pixels});
    RenderCommand frame(FrameCommand{Frame(1, 3)});
    RenderCommand release(ReleaseImageCommand{version});
    pixels.reset();

    assert(queue.TryPush(std::move(registration)) == QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(frame), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPush(std::move(release)) == QueuePushResult::Accepted);
    assert(!retained.expired());

    RenderCommand after_barrier(FrameCommand{Frame(1, 4)});
    assert(queue.TryPushLatest(std::move(after_barrier), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Busy);
    assert(std::get<FrameCommand>(after_barrier).frame->scene_revision == 4);

    auto first = queue.TryPop();
    auto second = queue.TryPop();
    auto third = queue.TryPop();
    assert(first && second && third && !queue.TryPop());
    assert(std::get<RegisterImageCommand>(*first).version == version);
    assert(std::get<FrameCommand>(*second).frame->scene_revision == 3);
    assert(std::get<ReleaseImageCommand>(*third).version == version);
    first.reset();
    assert(retained.expired());
}

void VerifyInstallAndRequestBarriers()
{
    PollableQueue<RenderCommand> queue(6);
    RenderCommand before(FrameCommand{Frame(1, 1)});
    RenderCommand install(InstallUiCommand{{1, 2}});
    RenderCommand after(FrameCommand{Frame(2, 2)});
    RenderCommand request(RequestRenderCommand{RenderRequestKind::Redraw, true});
    RenderCommand processed(UiEventsProcessedCommand{21});
    RenderCommand latest(FrameCommand{Frame(2, 3)});
    RenderCommand replacement(FrameCommand{Frame(2, 4)});

    assert(queue.TryPushLatest(std::move(before), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPush(std::move(install)) == QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(after), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPush(std::move(processed)) == QueuePushResult::Accepted);
    assert(queue.TryPush(std::move(request)) == QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(latest), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(replacement), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Replaced);

    auto first = queue.TryPop();
    auto second = queue.TryPop();
    auto third = queue.TryPop();
    auto fourth = queue.TryPop();
    auto fifth = queue.TryPop();
    auto sixth = queue.TryPop();
    assert(first && second && third && fourth && fifth && sixth && !queue.TryPop());
    assert(std::get<FrameCommand>(*first).frame->sequence == 1);
    assert((std::get<InstallUiCommand>(*second).ui == prism::runtime::UiLoadId{1, 2}));
    assert(std::get<FrameCommand>(*third).frame->sequence == 2);
    assert(std::get<UiEventsProcessedCommand>(*fourth).sequence == 21);
    assert(std::get<RequestRenderCommand>(*fifth).kind == RenderRequestKind::Redraw);
    assert(std::get<RequestRenderCommand>(*fifth).deferred);
    assert(std::get<FrameCommand>(*sixth).frame->sequence == 4);
}

void VerifyInvalidationBarrier()
{
    PollableQueue<RenderCommand> queue(3);
    RenderCommand stale(FrameCommand{Frame(1, 10)});
    RenderCommand invalidate(InvalidateFrameCommand{{1, 1}});
    RenderCommand candidate(FrameCommand{Frame(1, 11)});
    RenderCommand latest(FrameCommand{Frame(1, 12)});

    assert(queue.TryPushLatest(std::move(stale), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPush(std::move(invalidate)) == QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(candidate), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(latest), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Replaced);

    auto first = queue.TryPop();
    auto second = queue.TryPop();
    auto third = queue.TryPop();
    assert(first && second && third && !queue.TryPop());
    assert(std::get<FrameCommand>(*first).frame->sequence == 10);
    assert((std::get<InvalidateFrameCommand>(*second).ui == prism::runtime::UiLoadId{1, 1}));
    assert(std::get<FrameCommand>(*third).frame->sequence == 12);
}

void VerifyOrderedStatusReplacement()
{
    PollableQueue<RenderEvent> queue(3);
    RenderStatusEvent first_status;
    first_status.platform.configure_count = 1;
    RenderStatusEvent latest_status;
    latest_status.platform.configure_count = 2;
    RenderStatusEvent after_configure;
    after_configure.platform.configure_count = 3;
    prism::contracts::ConfigureEvent configure;
    configure.configure_count = 3;

    RenderEvent first(std::move(first_status));
    RenderEvent latest(std::move(latest_status));
    RenderEvent barrier(SequencedWindowEvent{prism::contracts::WindowEvent{configure}, 19});
    RenderEvent last(std::move(after_configure));

    assert(queue.TryPushLatest(std::move(first), prism::runtime::ReplaceStatusTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(latest), prism::runtime::ReplaceStatusTail) ==
           QueuePushResult::Replaced);
    assert(queue.TryPushLatest(std::move(barrier), prism::runtime::ReplaceStatusTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(last), prism::runtime::ReplaceStatusTail) ==
           QueuePushResult::Accepted);

    auto old_status = queue.TryPop();
    auto ordered_configure = queue.TryPop();
    auto new_status = queue.TryPop();
    assert(old_status && ordered_configure && new_status && !queue.TryPop());
    assert(std::get<RenderStatusEvent>(*old_status).platform.configure_count == 2);
    assert(std::get<SequencedWindowEvent>(*ordered_configure).sequence == 19);
    assert(std::get<prism::contracts::ConfigureEvent>(
               std::get<SequencedWindowEvent>(*ordered_configure).event)
               .configure_count == 3);
    assert(std::get<RenderStatusEvent>(*new_status).platform.configure_count == 3);
}

void VerifySubmittedPacketIdentity()
{
    auto mutable_packet = std::make_shared<prism::runtime::FramePacket>();
    mutable_packet->ui = {1, 1};
    mutable_packet->sequence = 7;
    assert(mutable_packet->animation_sample.revision == 0 &&
           mutable_packet->animation_sample.time_ns == 0);
    mutable_packet->animation_sample = {2, 1'050'000'000};
    std::shared_ptr<const prism::runtime::FramePacket> submitted = mutable_packet;
    prism::runtime::SubmittedFrameEvent result;
    result.ui = submitted->ui;
    result.frame_sequence = submitted->sequence;
    result.frame = submitted;

    const auto later_candidate = Frame(1, 8);
    assert(result.frame == submitted);
    assert(result.frame != later_candidate);
    assert(result.frame_sequence == result.frame->sequence);
    assert(result.frame->animation_sample.revision == 2);
    assert(result.frame->animation_sample.time_ns == 1'050'000'000);
}

void VerifyFrameOpportunityOrdering()
{
    PollableQueue<RenderCommand> commands(5);
    const prism::runtime::UiLoadId ui{1, 3};
    const prism::runtime::RenderWorkerGeneration worker{2};
    const auto packet = Frame(3, 11);
    RenderCommand sampling(SetAnimationSamplingCommand{ui, true});
    RenderCommand candidate(FrameCommand{packet});
    RenderCommand ready(AnswerFrameOpportunityCommand{ui, worker, 4, 7, packet});
    RenderCommand later(FrameCommand{Frame(3, 12)});

    assert(commands.TryPush(std::move(sampling)) == QueuePushResult::Accepted);
    assert(commands.TryPushLatest(std::move(candidate), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);
    assert(commands.TryPush(std::move(ready)) == QueuePushResult::Accepted);
    assert(commands.TryPushLatest(std::move(later), prism::runtime::ReplaceFrameTail) ==
           QueuePushResult::Accepted);

    assert(std::holds_alternative<SetAnimationSamplingCommand>(*commands.TryPop()));
    assert(std::get<FrameCommand>(*commands.TryPop()).frame == packet);
    const auto response = std::get<AnswerFrameOpportunityCommand>(*commands.TryPop());
    assert(response.ui == ui && response.worker == worker && response.configure_count == 4);
    assert(response.id == 7 && response.frame == packet);
    assert(std::get<FrameCommand>(*commands.TryPop()).frame->sequence == 12);

    PollableQueue<RenderEvent> events(3);
    RenderEvent before(RenderStatusEvent{});
    RenderEvent opportunity(FrameOpportunityEvent{ui, worker, 4, 7});
    RenderEvent after(RenderStatusEvent{});
    assert(events.TryPushLatest(std::move(before), prism::runtime::ReplaceStatusTail) ==
           QueuePushResult::Accepted);
    assert(events.TryPushLatest(std::move(opportunity), prism::runtime::ReplaceStatusTail) ==
           QueuePushResult::Accepted);
    assert(events.TryPushLatest(std::move(after), prism::runtime::ReplaceStatusTail) ==
           QueuePushResult::Accepted);
    assert(std::holds_alternative<RenderStatusEvent>(*events.TryPop()));
    assert(std::get<FrameOpportunityEvent>(*events.TryPop()).id == 7);
    assert(std::holds_alternative<RenderStatusEvent>(*events.TryPop()));
}

void VerifyConfigureReplacesOpportunityGeneration()
{
    PollableQueue<RenderEvent> events(3);
    const prism::runtime::UiLoadId ui{1, 3};
    const prism::runtime::RenderWorkerGeneration worker{2};
    prism::contracts::ConfigureEvent configure;
    configure.configure_count = 5;

    RenderEvent old(FrameOpportunityEvent{ui, worker, 4, 10});
    RenderEvent resized(SequencedWindowEvent{prism::contracts::WindowEvent{configure}, 20});
    RenderEvent current(FrameOpportunityEvent{ui, worker, 5, 11});
    assert(events.TryPush(std::move(old)) == QueuePushResult::Accepted);
    assert(events.TryPush(std::move(resized)) == QueuePushResult::Accepted);
    assert(events.TryPush(std::move(current)) == QueuePushResult::Accepted);

    const auto first = std::get<FrameOpportunityEvent>(*events.TryPop());
    const auto middle = std::get<SequencedWindowEvent>(*events.TryPop());
    const auto last = std::get<FrameOpportunityEvent>(*events.TryPop());
    assert(first.configure_count == 4 && first.id == 10);
    assert(std::get<prism::contracts::ConfigureEvent>(middle.event).configure_count == 5);
    assert(last.configure_count == 5 && last.id == 11);
}

RenderEvent Motion(std::uint64_t sequence, prism::contracts::InputSource source = {1, 1, 1},
                   prism::runtime::UiLoadId ui = {1, 1}, prism::contracts::WindowId window = {1},
                   std::shared_ptr<const prism::runtime::InputSnapshot> snapshot = {})
{
    prism::contracts::PointerMotionEvent motion{
        window, {static_cast<double>(sequence), 2}, sequence * 1000, source};
    return SequencedWindowEvent{motion, sequence, ui, std::move(snapshot)};
}

void VerifyMotionIdentityAndBarriers()
{
    PollableQueue<RenderEvent> queue(3);
    auto first = Motion(1);
    auto latest = Motion(2);
    assert(queue.TryPushLatest(std::move(first), prism::runtime::ReplacePointerMotionTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(latest), prism::runtime::ReplacePointerMotionTail) ==
           QueuePushResult::Replaced);
    const auto merged = std::get<SequencedWindowEvent>(*queue.TryPop());
    assert(merged.sequence == 2 && merged.ui == (prism::runtime::UiLoadId{1, 1}));
    assert(std::get<prism::contracts::PointerMotionEvent>(merged.event).position.x == 2);
    assert(!queue.TryPop());

    const auto original = Motion(1);
    assert(!prism::runtime::ReplacePointerMotionTail(original, Motion(2, {2, 1, 1})));
    assert(!prism::runtime::ReplacePointerMotionTail(original, Motion(2, {1, 2, 1})));
    assert(!prism::runtime::ReplacePointerMotionTail(original, Motion(2, {1, 1, 2})));
    assert(!prism::runtime::ReplacePointerMotionTail(original, Motion(2, {1, 1, 1}, {1, 2})));
    assert(!prism::runtime::ReplacePointerMotionTail(original, Motion(2, {1, 1, 1}, {2, 1})));
    assert(!prism::runtime::ReplacePointerMotionTail(original, Motion(2, {1, 1, 1}, {1, 1}, {2})));

    const prism::contracts::WindowEvent barriers[]{
        prism::contracts::PointerEnterEvent{{1}, {2, 3}, 1, {1, 1, 1}},
        prism::contracts::PointerLeaveEvent{{1}, 1, {1, 1, 1}},
        prism::contracts::PointerCancelEvent{{1}, 1, {1, 1, 1}},
        prism::contracts::PointerButtonEvent{{1},
                                             {2, 3},
                                             prism::contracts::PointerButton::Primary,
                                             prism::contracts::ButtonState::Pressed},
        prism::contracts::PointerButtonEvent{{1},
                                             {2, 3},
                                             prism::contracts::PointerButton::Primary,
                                             prism::contracts::ButtonState::Released},
        prism::contracts::FocusEvent{{1}, false, {1, 2, 1}},
        prism::contracts::ConfigureEvent{{1}},
    };
    for (const auto &barrier : barriers) {
        auto before = Motion(10);
        RenderEvent ordered(SequencedWindowEvent{barrier, 11, {1, 1}});
        auto after = Motion(12);
        assert(queue.TryPushLatest(std::move(before), prism::runtime::ReplacePointerMotionTail) ==
               QueuePushResult::Accepted);
        assert(queue.TryPushLatest(std::move(ordered), prism::runtime::ReplacePointerMotionTail) ==
               QueuePushResult::Accepted);
        assert(queue.TryPushLatest(std::move(after), prism::runtime::ReplacePointerMotionTail) ==
               QueuePushResult::Accepted);
        assert(std::get<SequencedWindowEvent>(*queue.TryPop()).sequence == 10);
        assert(std::get<SequencedWindowEvent>(*queue.TryPop()).event.index() == barrier.index());
        assert(std::get<SequencedWindowEvent>(*queue.TryPop()).sequence == 12);
        assert(!queue.TryPop());
    }
}

RenderEvent TouchMotion(std::uint64_t sequence, prism::contracts::InputContactId contact,
                        const std::shared_ptr<const prism::runtime::InputSnapshot> &snapshot,
                        prism::contracts::InputSource source = {1, 3, 1},
                        prism::runtime::UiLoadId ui = {1, 1},
                        prism::contracts::WindowId window = {1})
{
    prism::contracts::TouchMotionEvent motion{
        window, {static_cast<double>(sequence), 2}, contact, sequence * 1000, source};
    return SequencedWindowEvent{motion, sequence, ui, snapshot};
}

void VerifyMotionSnapshotIsolation()
{
    const auto old_snapshot = InputGeometry(1);
    const auto current_snapshot = InputGeometry(2);
    const auto same_version = InputGeometry(1);
    const auto other_scene = InputGeometry(1, 2);
    PollableQueue<RenderEvent> queue(2);
    auto first = Motion(1, {1, 1, 1}, {1, 1}, {1}, old_snapshot);
    auto latest = Motion(2, {1, 1, 1}, {1, 1}, {1}, old_snapshot);
    auto changed = Motion(3, {1, 1, 1}, {1, 1}, {1}, current_snapshot);
    assert(queue.TryPushLatest(std::move(first), prism::runtime::ReplacePointerMotionTail) ==
           QueuePushResult::Accepted);
    assert(queue.TryPushLatest(std::move(latest), prism::runtime::ReplacePointerMotionTail) ==
           QueuePushResult::Replaced);
    assert(queue.TryPushLatest(std::move(changed), prism::runtime::ReplacePointerMotionTail) ==
           QueuePushResult::Accepted);
    const auto before = std::get<SequencedWindowEvent>(*queue.TryPop());
    const auto after = std::get<SequencedWindowEvent>(*queue.TryPop());
    assert(before.sequence == 2 && before.input_snapshot == old_snapshot);
    assert(after.sequence == 3 && after.input_snapshot == current_snapshot);

    const auto original = Motion(1, {1, 1, 1}, {1, 1}, {1}, old_snapshot);
    for (const auto &snapshot : {current_snapshot, same_version, other_scene,
                                 std::shared_ptr<const prism::runtime::InputSnapshot>{}}) {
        assert(!prism::runtime::ReplacePointerMotionTail(
            original, Motion(2, {1, 1, 1}, {1, 1}, {1}, snapshot)));
    }
}

void VerifyTouchContactAndSnapshotBarriers()
{
    const auto snapshot = InputGeometry(3);
    const auto original = TouchMotion(1, 4, snapshot);
    assert(prism::runtime::ReplacePointerMotionTail(original, TouchMotion(2, 4, snapshot)));
    assert(!prism::runtime::ReplacePointerMotionTail(original, TouchMotion(2, 5, snapshot)));
    assert(
        !prism::runtime::ReplacePointerMotionTail(original, TouchMotion(2, 4, InputGeometry(4))));
    assert(
        !prism::runtime::ReplacePointerMotionTail(original, TouchMotion(2, 4, InputGeometry(3))));
    assert(!prism::runtime::ReplacePointerMotionTail(original,
                                                     TouchMotion(2, 4, snapshot, {2, 3, 1})));
    assert(!prism::runtime::ReplacePointerMotionTail(original,
                                                     TouchMotion(2, 4, snapshot, {1, 4, 1})));
    assert(!prism::runtime::ReplacePointerMotionTail(original,
                                                     TouchMotion(2, 4, snapshot, {1, 3, 2})));
    assert(!prism::runtime::ReplacePointerMotionTail(
        original, TouchMotion(2, 4, snapshot, {1, 3, 1}, {1, 2})));
    assert(!prism::runtime::ReplacePointerMotionTail(
        original, TouchMotion(2, 4, snapshot, {1, 3, 1}, {1, 1}, {2})));
    assert(!prism::runtime::ReplacePointerMotionTail(original,
                                                     Motion(2, {1, 3, 1}, {1, 1}, {1}, snapshot)));

    const prism::contracts::WindowEvent barriers[]{
        prism::contracts::TouchDownEvent{{1}, {2, 3}, 4, 1000, {1, 3, 1}},
        prism::contracts::TouchUpEvent{{1}, 4, 1000, {1, 3, 1}},
        prism::contracts::TouchCancelEvent{{1}, 1000, {1, 3, 1}},
        prism::contracts::TouchFrameEvent{{1}, 1000, {1, 3, 1}},
    };
    PollableQueue<RenderEvent> queue(3);
    for (const auto &barrier : barriers) {
        auto before = TouchMotion(10, 4, snapshot);
        RenderEvent ordered(SequencedWindowEvent{barrier, 11, {1, 1}, snapshot});
        auto after = TouchMotion(12, 4, snapshot);
        assert(queue.TryPushLatest(std::move(before), prism::runtime::ReplacePointerMotionTail) ==
               QueuePushResult::Accepted);
        assert(queue.TryPushLatest(std::move(ordered), prism::runtime::ReplacePointerMotionTail) ==
               QueuePushResult::Accepted);
        assert(queue.TryPushLatest(std::move(after), prism::runtime::ReplacePointerMotionTail) ==
               QueuePushResult::Accepted);
        assert(std::get<SequencedWindowEvent>(*queue.TryPop()).sequence == 10);
        assert(std::get<SequencedWindowEvent>(*queue.TryPop()).event.index() == barrier.index());
        assert(std::get<SequencedWindowEvent>(*queue.TryPop()).sequence == 12);
        assert(!queue.TryPop());
    }
}

void VerifyInputSnapshotLeases()
{
    auto current = InputGeometry(1);
    std::weak_ptr<const prism::runtime::InputSnapshot> old_reference = current;
    std::weak_ptr<const prism::runtime::InputSnapshot> new_reference;
    {
        PollableQueue<RenderEvent> events(1);
        auto event = Motion(1, {1, 1, 1}, {1, 1}, {1}, current);
        assert(events.TryPush(std::move(event)) == QueuePushResult::Accepted);
        current = InputGeometry(2);
        new_reference = current;
        assert(!old_reference.expired());
        assert(!new_reference.expired());

        auto consumed = events.TryPop();
        assert(consumed && !old_reference.expired());
        consumed.reset();
        assert(old_reference.expired());
        assert(!new_reference.expired());

        auto remaining = TouchMotion(2, 7, current);
        assert(events.TryPush(std::move(remaining)) == QueuePushResult::Accepted);
        current.reset();
        assert(!new_reference.expired());
        assert(events.Close());
        assert(!new_reference.expired());
    }
    assert(new_reference.expired());

    std::weak_ptr<const prism::runtime::InputSnapshot> discarded;
    std::weak_ptr<const prism::runtime::InputSnapshot> candidate;
    {
        PollableQueue<RenderCommand> commands(1);
        auto packet = std::make_shared<prism::runtime::FramePacket>();
        packet->ui = {1, 1};
        packet->input_snapshot = InputGeometry(3);
        discarded = packet->input_snapshot;
        RenderCommand first(FrameCommand{std::move(packet)});
        assert(commands.TryPushLatest(std::move(first), prism::runtime::ReplaceFrameTail) ==
               QueuePushResult::Accepted);
        assert(!discarded.expired());

        packet = std::make_shared<prism::runtime::FramePacket>();
        packet->ui = {1, 1};
        packet->input_snapshot = InputGeometry(4);
        candidate = packet->input_snapshot;
        RenderCommand replacement(FrameCommand{std::move(packet)});
        assert(commands.TryPushLatest(std::move(replacement), prism::runtime::ReplaceFrameTail) ==
               QueuePushResult::Replaced);
        assert(discarded.expired() && !candidate.expired());
    }
    assert(candidate.expired());
}
} // namespace

int main()
{
    VerifyFrameReplacement();
    VerifyResourceBarriersAndLease();
    VerifyInstallAndRequestBarriers();
    VerifyInvalidationBarrier();
    VerifyOrderedStatusReplacement();
    VerifySubmittedPacketIdentity();
    VerifyFrameOpportunityOrdering();
    VerifyConfigureReplacesOpportunityGeneration();
    VerifyMotionIdentityAndBarriers();
    VerifyMotionSnapshotIsolation();
    VerifyTouchContactAndSnapshotBarriers();
    VerifyInputSnapshotLeases();
}
