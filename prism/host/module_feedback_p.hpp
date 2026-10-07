#pragma once

#include "prism/sdk/module_session.hpp"

#include <optional>
#include <utility>

namespace prism::sdk {
struct ModuleFeedbackState {
    ModuleFeedbackState(ModuleSession::FeedbackSink submit,
                        ModuleSession::FeedbackDismissSink dismiss,
                        ModuleSession::FeedbackCapabilitySink capabilities)
        : submit(std::move(submit)), dismiss(std::move(dismiss)),
          capabilities(std::move(capabilities))
    {
    }

    ModuleSession::FeedbackSink submit;
    ModuleSession::FeedbackDismissSink dismiss;
    ModuleSession::FeedbackCapabilitySink capabilities;
    std::optional<contracts::OwnerFeedbackRequest> pending;
    std::uint64_t last_request{};
    bool submitting{}, querying{};
};
} // namespace prism::sdk
