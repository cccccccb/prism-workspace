#include "client_render_owner_p.hpp"

#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

using namespace prism;

namespace {
// The fixture never opens Wayland or EGL. Only null checks may inspect these
// sentinels, and their RAII scope removes them before native object teardown.
class PendingCallbackScope {
public:
    explicit PendingCallbackScope(wl_callback *&callback) : callback_(callback), previous_(callback)
    {
        callback_ = reinterpret_cast<wl_callback *>(this);
    }

    ~PendingCallbackScope()
    {
        callback_ = previous_;
    }

    PendingCallbackScope(const PendingCallbackScope &) = delete;
    PendingCallbackScope &operator=(const PendingCallbackScope &) = delete;

private:
    wl_callback *&callback_;
    wl_callback *previous_;
};

struct Fixture {
    runtime::PollableQueue<runtime::RenderCommand> commands{32};
    runtime::PollableQueue<runtime::RenderEvent> events{32};
    runtime::TerminalSignal terminal;
    runtime::RenderWorkerLifecycle lifecycle{terminal};
    sdk::ClientRenderOwner owner{sdk::ClientConfig{}, commands, events, terminal, lifecycle};
    std::shared_ptr<const runtime::FramePacket> frame;
    std::shared_ptr<const runtime::PopupFramePacket> child;

    Fixture()
    {
        const runtime::UiLoadId ui{41, 3};
        const runtime::RenderWorkerGeneration worker{7};
        owner.installed_ui_ = ui;
        owner.worker_generation_ = worker;
        owner.animation_sampling_active_ = true;
        owner.window_.configure_count_ = 4;
        owner.window_.configured_ = true;
        owner.window_.metrics_ = {{320, 200}, {320, 200}, 1};
        owner.popup_.closed_ = false;
        owner.popup_.surface_lifetime_id_ = 19;
        owner.popup_.configure_ = platform::WaylandPopupConfigure{{40, 50, 160, 80}, 17, 2};

        auto request = std::make_shared<runtime::PopupSurfaceRequest>();
        request->scene = 11;
        request->popup_token = 23;
        request->active_node = {1, 1};
        request->trigger = {2, 1};
        request->parent_configure_generation = 4;
        request->parent_window_geometry = {0, 0, 320, 200};
        request->anchor = {40, 10, 80, 32};
        request->desired_body = {160, 80};
        request->desired_geometry = {160, 80};

        auto input = std::make_shared<runtime::InputSnapshot>();
        input->scene = request->scene;
        input->version = 1;
        input->popup_token = request->popup_token;
        input->root = {0, 1};
        input->viewport = {320, 200};
        input->nodes.resize(1);
        input->nodes[0].id = input->root;
        input->nodes[0].bounds = {0, 0, 320, 200};
        input->nodes[0].visible = true;
        input->nodes[0].enabled = true;

        auto list = std::make_shared<contracts::DisplayList>();
        list->commands.push_back(contracts::FillRect{{0, 0, 320, 200}, {25, 42, 59, 255}});
        auto uses = std::make_shared<const std::vector<runtime::ImageVersion>>();
        auto plan = std::make_shared<runtime::PopupSurfacePlan>();
        plan->request = *request;
        plan->configure_generation = 2;
        plan->buffer_size = {160, 80};
        plan->window_geometry = {0, 0, 160, 80};
        plan->display_list = list;

        auto popup = std::make_shared<runtime::PopupFramePacket>();
        popup->ui = ui;
        popup->worker = worker;
        popup->identity = {7, 19, 19, 2, 0};
        popup->sequence = 20;
        popup->plan = std::move(plan);
        popup->image_uses = uses;
        child = std::move(popup);

        auto root = std::make_shared<runtime::FramePacket>();
        root->ui = ui;
        root->sequence = 20;
        root->configure_count = 4;
        root->buffer_size = {320, 200};
        root->display_list = std::move(list);
        root->image_uses = std::move(uses);
        root->input_snapshot = std::move(input);
        root->popup_surface_request = request;
        root->popup_surface_frame = child;
        frame = std::move(root);

        owner.root_target_.render_frame = frame;
        owner.root_target_.committed_frame = frame;
        owner.popup_ui_ = ui;
        owner.popup_request_ = std::move(request);
        owner.popup_frame_ = child;
        owner.popup_committed_frame_ = child;
        owner.popup_clean_parent_ = true;
        owner.popup_effects_ready_ = true;
        assert(owner.root_target_.render_frame->display_list ==
               owner.root_target_.committed_frame->display_list);
    }

    ~Fixture()
    {
        assert(!owner.window_.frame_callback_ && !owner.popup_.frame_callback_);
        // This was only a configured-state fixture, never a real native child.
        owner.popup_.closed_ = true;
        owner.popup_.configure_.reset();
    }

    platform::SubmitResult Prepare(bool allow_pixels = true)
    {
        const platform::SubmitRequest request{nullptr, nullptr, 320, 200, allow_pixels, false};
        const auto result = owner.PrepareSubmit(request);
        assert(result != platform::SubmitResult::Failed);
        assert(terminal.Reason() == runtime::TerminalReason::None);
        return result;
    }

    runtime::FrameOpportunityEvent TakeOpportunity()
    {
        auto event = events.TryPop();
        assert(event);
        const auto *opportunity = std::get_if<runtime::FrameOpportunityEvent>(&*event);
        assert(opportunity && opportunity->id && opportunity->ui == frame->ui &&
               opportunity->worker == owner.worker_generation_ &&
               opportunity->configure_count == frame->configure_count);
        assert(!events.TryPop());
        assert(owner.frame_opportunity_ && owner.frame_opportunity_->id == opportunity->id);
        return *opportunity;
    }

    void ExpectNoOpportunity()
    {
        assert(!owner.frame_opportunity_);
        assert(!events.TryPop());
        assert(terminal.Reason() == runtime::TerminalReason::None);
    }
};

void MetadataCompletionRequestsNextSample()
{
    for (bool child_first : {false, true}) {
        Fixture f;
        f.owner.ApproveFrame(f.frame);
        assert(!f.owner.approved_root_consumed_ && !f.owner.approved_popup_consumed_);
        if (child_first) {
            f.owner.ConsumeApprovedPopup(f.child);
            assert(f.owner.approved_frame_);
            f.owner.ConsumeApprovedRoot(f.frame);
        } else {
            f.owner.ConsumeApprovedRoot(f.frame);
            assert(f.owner.approved_frame_);
            f.owner.ConsumeApprovedPopup(f.child);
        }
        assert(!f.owner.approved_frame_ && !f.owner.spontaneous_animation_frame_allowed_);
        assert(f.owner.window_.update_requested_);

        // Both targets consumed metadata; neither submitted a pixel callback.
        // The root list already matches its baseline, which formerly selected
        // a silent child clock and parked Pump before the next AdvancePopup.
        assert(f.Prepare() == platform::SubmitResult::None);
        const auto first = f.TakeOpportunity();
        assert(f.Prepare() == platform::SubmitResult::None);
        assert(!f.events.TryPop());
        assert(f.owner.frame_opportunity_->id == first.id); // Exactly one permit.
    }
}

void CallbackAndApprovalBarriers()
{
    {
        Fixture f;
        PendingCallbackScope pending(f.owner.window_.frame_callback_);
        assert(f.Prepare(false) == platform::SubmitResult::None);
        f.ExpectNoOpportunity();
        assert(f.owner.AdvancePopup());
        f.ExpectNoOpportunity();
    }
    {
        Fixture f;
        PendingCallbackScope pending(f.owner.popup_.frame_callback_);
        assert(f.Prepare() == platform::SubmitResult::None);
        f.ExpectNoOpportunity();
        assert(f.owner.AdvancePopup());
        f.ExpectNoOpportunity();
    }
    for (bool child_consumed : {false, true}) {
        Fixture f;
        f.owner.ApproveFrame(f.frame);
        if (child_consumed) {
            f.owner.ConsumeApprovedPopup(f.child);
        } else {
            f.owner.ConsumeApprovedRoot(f.frame);
        }
        assert(f.owner.approved_frame_);
        assert(f.Prepare() == platform::SubmitResult::None);
        f.ExpectNoOpportunity();
    }
}

void InvalidCandidateWaitsForChildCallback()
{
    Fixture f;
    f.owner.root_target_.render_frame.reset();
    {
        PendingCallbackScope pending(f.owner.popup_.frame_callback_);
        assert(f.Prepare() == platform::SubmitResult::AwaitFrame);
        f.ExpectNoOpportunity();
    }
    assert(f.Prepare() == platform::SubmitResult::AwaitFrame);
    f.TakeOpportunity();
}

void ChildReadyWaitsForRootCallback()
{
    Fixture f;
    const platform::WaylandPopupFrameReady ready{f.owner.popup_.Target(), {11}};
    {
        PendingCallbackScope pending(f.owner.window_.frame_callback_);
        f.owner.QueuePopupEvent(ready);
        f.ExpectNoOpportunity();
    }
    f.owner.QueuePopupEvent(ready);
    f.TakeOpportunity();
}

void ReadyChildClockRequestsOneSample()
{
    Fixture f;
    assert(f.owner.AdvancePopup());
    const auto first = f.TakeOpportunity();
    assert(f.owner.AdvancePopup());
    assert(!f.events.TryPop());
    assert(f.owner.frame_opportunity_->id == first.id);
}
} // namespace

int main()
{
    MetadataCompletionRequestsNextSample();
    CallbackAndApprovalBarriers();
    InvalidCandidateWaitsForChildCallback();
    ChildReadyWaitsForRootCallback();
    ReadyChildClockRequestsOneSample();
    std::puts("client_frame_opportunity_test: passed");
}
