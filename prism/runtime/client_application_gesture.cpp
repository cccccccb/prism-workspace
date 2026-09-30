#include "client_application_p.hpp"

namespace prism::sdk {

void ClientApplication::OnGesture(std::function<void(const contracts::GestureEvent &)> callback)
{
    impl_->on_gesture = std::move(callback);
}

void ClientApplication::Impl::CollectGestureEvents()
{
    if (!scene) {
        return;
    }
    auto events = scene->TakeGestureEvents();
    if (pending_gestures.size() + events.size() > 1024) {
        throw std::length_error("Gesture callback queue limit exceeded");
    }
    for (auto &event : events) {
        pending_gestures.push_back({installed_ui, std::move(event)});
    }
}

void ClientApplication::Impl::DeliverGestureEvents()
{
    CollectGestureEvents();
    if (delivering_gestures) {
        return;
    }

    struct DeliveryGuard {
        bool &active;

        ~DeliveryGuard()
        {
            active = false;
        }
    } guard{delivering_gestures};

    delivering_gestures = true;

    while (!pending_gestures.empty()) {
        auto pending = std::move(pending_gestures.front());
        pending_gestures.pop_front();
        const auto &event = pending.event;
        if (event.phase == contracts::GesturePhase::Begin) {
            if (pending.ui != installed_ui || closed || failed || !on_gesture) {
                continue;
            }
            delivered_gestures.insert(event.id);
        } else if (event.phase == contracts::GesturePhase::Update) {
            if (pending.ui != installed_ui || closed || failed ||
                !delivered_gestures.contains(event.id)) {
                continue;
            }
        } else if (!delivered_gestures.erase(event.id)) {
            continue;
        }

        // Neither Scene references nor callbacks stored in Impl survive a user
        // callback: it may replace the UI, mutate bindings or change the handler.
        auto callback = on_gesture;
        if (callback) {
            callback(event);
        }
        CollectGestureEvents();
    }
}
} // namespace prism::sdk
