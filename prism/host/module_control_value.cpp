#include "prism/sdk/module_session.hpp"

namespace prism::sdk {
namespace {
PrismValueV1 BorrowValue(const runtime::ControlValue &value)
{
    PrismValueV1 result{};
    if (const auto *boolean = std::get_if<bool>(&value)) {
        result.kind = PRISM_VALUE_BOOL_V1;
        result.as.boolean = *boolean ? 1 : 0;
    } else if (const auto *number = std::get_if<double>(&value)) {
        result.kind = PRISM_VALUE_NUMBER_V1;
        result.as.number = *number;
    } else {
        const auto &text = std::get<std::string>(value);
        result.kind = PRISM_VALUE_STRING_V1;
        result.as.string = {text.data(), text.size()};
    }
    return result;
}

std::uint32_t Phase(runtime::ValuePhase phase)
{
    switch (phase) {
    case runtime::ValuePhase::Preview:
        return PRISM_CONTROL_PREVIEW_V1;
    case runtime::ValuePhase::Commit:
        return PRISM_CONTROL_COMMIT_V1;
    case runtime::ValuePhase::Cancel:
        return PRISM_CONTROL_CANCEL_V1;
    }
    return PRISM_CONTROL_CANCEL_V1;
}

std::uint32_t Reason(runtime::ValueCancelReason reason)
{
    switch (reason) {
    case runtime::ValueCancelReason::None:
        return PRISM_CONTROL_CANCEL_NONE_V1;
    case runtime::ValueCancelReason::Escape:
        return PRISM_CONTROL_CANCEL_ESCAPE_V1;
    case runtime::ValueCancelReason::Unavailable:
        return PRISM_CONTROL_CANCEL_UNAVAILABLE_V1;
    case runtime::ValueCancelReason::FocusLost:
        return PRISM_CONTROL_CANCEL_FOCUS_LOST_V1;
    case runtime::ValueCancelReason::Superseded:
        return PRISM_CONTROL_CANCEL_SUPERSEDED_V1;
    }
    return PRISM_CONTROL_CANCEL_UNAVAILABLE_V1;
}
} // namespace

void ModuleSession::ControlValue(const runtime::ControlEdit &edit)
{
    if (!OnOwnerThread() || closed_ || !instance_ || !module_.Api().on_control_value) {
        return;
    }

    const auto &value = edit.event;
    const PrismControlValueEventV1 event{sizeof(PrismControlValueEventV1),
                                         edit.node.index,
                                         edit.node.generation,
                                         Phase(value.phase),
                                         Reason(value.reason),
                                         value.interaction,
                                         value.revision,
                                         {edit.action.data(), edit.action.size()},
                                         BorrowValue(value.before),
                                         BorrowValue(value.value)};

    module_.Api().on_control_value(instance_, &event);
}
} // namespace prism::sdk
