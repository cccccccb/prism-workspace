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
}
