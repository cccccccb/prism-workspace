#pragma once

#include "prism/sdk/module_session.hpp"

#include <optional>
#include <utility>

namespace prism::sdk {
struct ModuleTaskState {
    ModuleTaskState(ModuleSession::TaskSink submit, ModuleSession::TaskCancelSink cancel,
                    ModuleSession::TaskCapabilitySink capabilities)
        : submit(std::move(submit)), cancel(std::move(cancel)),
          capabilities(std::move(capabilities))
    {
    }

    ModuleSession::TaskSink submit;
    ModuleSession::TaskCancelSink cancel;
    ModuleSession::TaskCapabilitySink capabilities;
    std::optional<contracts::OwnerTaskRequest> pending;
    std::uint64_t last_request{};
    bool submitting{};
    bool cancel_requested{};
};
} // namespace prism::sdk
