#include "client_application_p.hpp"

namespace prism::sdk {
runtime::ValueCancelReason
ClientApplication::Impl::ValidateControlDelivery(runtime::UiLoadId ui,
                                                 const runtime::ControlEdit &edit) const
{
    if (ui != installed_ui || closed || failed || !scene) {
        return runtime::ValueCancelReason::Unavailable;
    }
    return scene->ControlEditInvalidation(edit);
}

void ClientApplication::Impl::CollectControlEvents()
{
    if (!scene) {
        return;
    }
    for (auto &edit : scene->TakeControlEvents()) {
        control_delivery.Enqueue(installed_ui, std::move(edit));
    }
}

void ClientApplication::Impl::DeliverInteractionEvents()
{
    if (delivering_gestures) {
        return;
    }

    CollectControlEvents();
    // Gesture callbacks may replace the UI before the pending value is delivered.
    DeliverGestureEvents();
    control_delivery.Drain(std::bind_front(&Impl::ValidateControlDelivery, this));
}
} // namespace prism::sdk
