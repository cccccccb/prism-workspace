#include "app_host_p.hpp"

namespace prism::sdk {
std::uint32_t AppHost::Impl::OwnerFeedbackCapabilities() const
{
    if (failed || closed || !frontend || !frontend->SupportsOwnerFeedback()) {
        return 0;
    }
    return PRISM_FEEDBACK_CAP_OWNER_V1;
}

bool AppHost::Impl::ShowOwnerFeedback(const contracts::OwnerFeedbackRequest &request)
{
    return OwnerFeedbackCapabilities() && frontend->ShowOwnerFeedback(request);
}

bool AppHost::Impl::DismissOwnerFeedback(std::uint64_t request_id)
{
    return !failed && !closed && frontend && frontend->DismissOwnerFeedback(request_id);
}

void AppHost::Impl::AdvanceOwnerFeedback()
{
    if (failed || closed || !business || !frontend || owner_feedback_delivered) {
        return;
    }

    const auto action = frontend->TakeOwnerFeedbackAction();
    if (!action) {
        return;
    }
    // The SDK has already retired the visible request. A business callback may
    // submit its successor or start a task without changing this action's ID.
    business_progress = true;
    owner_feedback_delivered = true;
    business->DeliverFeedbackAction(*action);
}
} // namespace prism::sdk
