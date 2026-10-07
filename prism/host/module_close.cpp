#include "prism/sdk/module_session.hpp"

#include <limits>
#include <utility>

namespace prism::sdk {
bool ModuleSession::RequestClose()
{
    if (!OnOwnerThread() || closed_ || !instance_) {
        return false;
    }
    if (close_accepted_) {
        return true;
    }
    if (close_callback_active_ || pending_close_ || close_decision_) {
        return false;
    }

    const auto &api = module_.Api();
    if (!api.on_close_request) {
        close_callback_active_ = true;
        bool accepted = true;
        try {
            accepted = !api.on_close_requested || api.on_close_requested(instance_) != 0;
        } catch (...) {
            accepted = false;
        }
        close_callback_active_ = false;
        close_accepted_ = accepted && !closed_;
        return close_accepted_;
    }
    if (!next_close_id_) {
        return false;
    }

    const auto request_id = next_close_id_;
    next_close_id_ = request_id == std::numeric_limits<std::uint64_t>::max() ? 0 : request_id + 1;
    pending_close_ = request_id;
    const PrismCloseRequestV1 request{sizeof(request), request_id};
    close_callback_active_ = true;
    std::int32_t decision = PRISM_CLOSE_REJECT_V1;
    try {
        decision = api.on_close_request(instance_, &request);
    } catch (...) {
        decision = PRISM_CLOSE_REJECT_V1;
    }
    close_callback_active_ = false;
    if (closed_) {
        return false;
    }
    if (decision == PRISM_CLOSE_DEFER_V1) {
        return false;
    }

    // Direct and invalid returns retire the request. A conflicting synchronous
    // completion cannot override the callback's explicit return value.
    pending_close_.reset();
    close_decision_.reset();
    close_accepted_ = decision == PRISM_CLOSE_ACCEPT_V1;
    return close_accepted_;
}

std::int32_t ModuleSession::CompleteClose(void *context, std::uint64_t request_id,
                                          std::uint32_t decision) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return PRISM_CLOSE_WRONG_THREAD_V1;
    }
    auto &self = *static_cast<ModuleSession *>(context);
    if (self.closed_) {
        return PRISM_CLOSE_CLOSED_V1;
    }
    if (!self.instance_ || !request_id || !self.pending_close_ ||
        *self.pending_close_ != request_id || self.close_decision_ ||
        (decision != PRISM_CLOSE_REJECT_V1 && decision != PRISM_CLOSE_ACCEPT_V1)) {
        return PRISM_CLOSE_INVALID_V1;
    }

    self.pending_close_.reset();
    self.close_decision_ = decision == PRISM_CLOSE_ACCEPT_V1;
    return PRISM_CLOSE_COMPLETED_V1;
}

std::optional<bool> ModuleSession::ConsumeCloseDecision() noexcept
{
    if (!OnOwnerThread() || closed_ || close_callback_active_) {
        return std::nullopt;
    }

    auto decision = std::exchange(close_decision_, {});
    if (decision && *decision) {
        close_accepted_ = true;
    }
    return decision;
}
} // namespace prism::sdk
