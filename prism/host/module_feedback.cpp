#include "module_feedback_p.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace prism::sdk {
namespace {
class FeedbackCall {
public:
    explicit FeedbackCall(bool &active) noexcept : active_(active)
    {
        active_ = true;
    }

    ~FeedbackCall()
    {
        active_ = false;
    }

private:
    bool &active_;
};

bool ValidView(PrismFeedbackStringViewV1 view, std::size_t maximum) noexcept
{
    return view.size <= maximum && (view.data || !view.size);
}

std::string Copy(PrismFeedbackStringViewV1 view)
{
    return view.size ? std::string(view.data, view.size) : std::string{};
}

std::optional<contracts::OwnerFeedbackRequest> CopyRequest(const PrismFeedbackRequestV1 *request)
{
    constexpr auto minimum =
        offsetof(PrismFeedbackRequestV1, duration_ms) + sizeof(request->duration_ms);
    if (!request || request->struct_size < minimum ||
        request->actions_size > contracts::kMaxOwnerFeedbackActions ||
        (!request->actions && request->actions_size) ||
        !ValidView(request->title, contracts::kMaxOwnerFeedbackTitleBytes) ||
        !ValidView(request->message, contracts::kMaxOwnerFeedbackMessageBytes)) {
        return std::nullopt;
    }
    for (std::size_t at = 0; at < request->actions_size; ++at) {
        const auto &action = request->actions[at];
        if (action.struct_size < sizeof(PrismFeedbackActionV1) ||
            !ValidView(action.label, contracts::kMaxOwnerFeedbackActionLabelBytes)) {
            return std::nullopt;
        }
    }

    contracts::OwnerFeedbackRequest owned;
    owned.request_id = 1;
    owned.kind = static_cast<contracts::OwnerFeedbackKind>(request->kind);
    owned.title = Copy(request->title);
    owned.message = Copy(request->message);
    owned.duration_ms = request->duration_ms;
    owned.actions.reserve(request->actions_size);
    for (std::size_t at = 0; at < request->actions_size; ++at) {
        const auto &action = request->actions[at];
        owned.actions.push_back({action.id, Copy(action.label)});
    }
    if (!contracts::ValidateOwnerFeedbackRequest(owned)) {
        return std::nullopt;
    }
    return owned;
}
} // namespace

bool ModuleSession::SupportsFeedback() const noexcept
{
    return OnOwnerThread() && !closed_ && feedback_->submit && feedback_->dismiss &&
           feedback_->capabilities;
}

std::uint32_t ModuleSession::FeedbackCapabilities(void *context) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return 0;
    }
    auto &self = *static_cast<ModuleSession *>(context);
    auto &state = *self.feedback_;
    if (!self.SupportsFeedback() || state.querying) {
        return 0;
    }

    try {
        const FeedbackCall querying(state.querying);
        const auto capabilities = state.capabilities();
        return self.closed_ ? 0 : capabilities & contracts::kOwnerFeedbackCapability;
    } catch (...) {
        return 0;
    }
}

std::uint64_t ModuleSession::ShowFeedback(void *context,
                                          const PrismFeedbackRequestV1 *request) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return 0;
    }
    auto &self = *static_cast<ModuleSession *>(context);
    auto &state = *self.feedback_;
    if (self.closed_ || !self.started_ || !self.instance_ || state.submitting || state.querying ||
        state.last_request == std::numeric_limits<std::uint64_t>::max()) {
        return 0;
    }

    std::uint64_t id{};
    std::optional<contracts::OwnerFeedbackRequest> previous;
    try {
        const FeedbackCall submitting(state.submitting);
        auto owned = CopyRequest(request);
        if (!owned || (!owned->actions.empty() && !self.module_.Api().on_feedback_action) ||
            !(FeedbackCapabilities(context) & contracts::kOwnerFeedbackCapability)) {
            return 0;
        }
        owned->request_id = state.last_request + 1;
        auto prepared_pending = *owned;
        id = owned->request_id;
        previous = std::move(state.pending);
        state.pending = std::move(prepared_pending);
        state.last_request = id;
        if (!state.submit(*owned) || self.closed_ || !state.pending ||
            state.pending->request_id != id) {
            if (!self.closed_) {
                state.pending = std::move(previous);
            }
            return 0;
        }
        return id;
    } catch (...) {
        if (id && !self.closed_) {
            try {
                const FeedbackCall submitting(state.submitting);
                state.dismiss(id);
            } catch (...) {
            }
            state.pending = std::move(previous);
        }
        return 0;
    }
}

std::int32_t ModuleSession::DismissFeedback(void *context, std::uint64_t id) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return PRISM_FEEDBACK_WRONG_THREAD_V1;
    }
    auto &self = *static_cast<ModuleSession *>(context);
    auto &state = *self.feedback_;
    if (self.closed_) {
        return PRISM_FEEDBACK_CLOSED_V1;
    }
    if (!self.instance_ || state.submitting || state.querying || !state.dismiss || !state.pending ||
        !id || state.pending->request_id != id) {
        return PRISM_FEEDBACK_INVALID_V1;
    }

    try {
        const FeedbackCall submitting(state.submitting);
        if (!state.dismiss(id) || self.closed_) {
            return self.closed_ ? PRISM_FEEDBACK_CLOSED_V1 : PRISM_FEEDBACK_INVALID_V1;
        }
        state.pending.reset();
        return PRISM_FEEDBACK_DISMISSED_V1;
    } catch (...) {
        return PRISM_FEEDBACK_INVALID_V1;
    }
}

bool ModuleSession::DeliverFeedbackAction(const contracts::OwnerFeedbackAction &action)
{
    if (!OnOwnerThread() || closed_ || !instance_ || !module_.Api().on_feedback_action ||
        feedback_->submitting || feedback_->querying || !feedback_->pending ||
        !contracts::ValidateOwnerFeedbackAction(action, *feedback_->pending)) {
        return false;
    }

    const auto owned = action;
    feedback_->pending.reset();
    const PrismFeedbackActionEventV1 projected{sizeof(projected), owned.request_id,
                                               owned.action_id};
    try {
        module_.Api().on_feedback_action(instance_, &projected);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace prism::sdk
