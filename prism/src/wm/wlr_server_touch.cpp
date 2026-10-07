#include "wlr_server_internal.hpp"

#include <limits>

namespace prism::wm {
WlrTouchBinding::WlrTouchBinding(WlrServer *owner, wlr_touch *device) : server(owner), touch(device)
{
    down.binding = this;
    down.listener.notify = Down;
    motion.binding = this;
    motion.listener.notify = Motion;
    up.binding = this;
    up.listener.notify = Up;
    cancel.binding = this;
    cancel.listener.notify = Cancel;
    frame.binding = this;
    frame.listener.notify = Frame;
    destroy.binding = this;
    destroy.listener.notify = Destroy;
    wl_signal_add(&touch->events.down, &down.listener);
    wl_signal_add(&touch->events.motion, &motion.listener);
    wl_signal_add(&touch->events.up, &up.listener);
    wl_signal_add(&touch->events.cancel, &cancel.listener);
    wl_signal_add(&touch->events.frame, &frame.listener);
    wl_signal_add(&touch->base.events.destroy, &destroy.listener);
}

WlrTouchBinding::~WlrTouchBinding()
{
    wl_list_remove(&down.listener.link);
    wl_list_remove(&motion.listener.link);
    wl_list_remove(&up.listener.link);
    wl_list_remove(&cancel.listener.link);
    wl_list_remove(&frame.listener.link);
    wl_list_remove(&destroy.listener.link);
}

void WlrTouchBinding::Down(wl_listener *listener, void *data)
{
    auto *binding = WlContainerOf<Listener>(listener, offsetof(Listener, listener))->binding;
    binding->server->HandleTouchDown(binding, *static_cast<wlr_touch_down_event *>(data));
}

void WlrTouchBinding::Motion(wl_listener *listener, void *data)
{
    auto *binding = WlContainerOf<Listener>(listener, offsetof(Listener, listener))->binding;
    binding->server->HandleTouchMotion(binding, *static_cast<wlr_touch_motion_event *>(data));
}

void WlrTouchBinding::Up(wl_listener *listener, void *data)
{
    auto *binding = WlContainerOf<Listener>(listener, offsetof(Listener, listener))->binding;
    const auto &event = *static_cast<wlr_touch_up_event *>(data);
    binding->server->HandleTouchUp(binding, event.time_msec, event.touch_id);
}

void WlrTouchBinding::Cancel(wl_listener *listener, void *data)
{
    auto *binding = WlContainerOf<Listener>(listener, offsetof(Listener, listener))->binding;
    const auto &event = *static_cast<wlr_touch_cancel_event *>(data);
    binding->server->HandleTouchCancel(binding, event.touch_id);
}

void WlrTouchBinding::Frame(wl_listener *listener, void *)
{
    auto *binding = WlContainerOf<Listener>(listener, offsetof(Listener, listener))->binding;
    binding->server->HandleTouchFrame();
}

void WlrTouchBinding::Destroy(wl_listener *listener, void *)
{
    auto *binding = WlContainerOf<Listener>(listener, offsetof(Listener, listener))->binding;
    binding->server->HandleTouchDestroy(binding);
}

void WlrServer::AttachTouchDevice(wlr_touch *touch)
{
    if (std::any_of(touches_.begin(), touches_.end(),
                    [touch](const auto &binding) { return binding->touch == touch; })) {
        return;
    }

    wlr_cursor_attach_input_device(cursor_, &touch->base);
    touches_.push_back(std::make_unique<WlrTouchBinding>(this, touch));
    wlr_seat_set_capabilities(seat_, seat_->capabilities | WL_SEAT_CAPABILITY_TOUCH);
    PRISM_LOG_INFO("WLR-INPUT", "Touch attached: %s",
                   touch->base.name ? touch->base.name : "unknown");
}

std::int32_t WlrServer::AllocateTouchId()
{
    const auto first = next_touch_id_;
    do {
        const auto candidate = next_touch_id_;
        next_touch_id_ = candidate == std::numeric_limits<std::int32_t>::max() ? 0 : candidate + 1;
        if (!wlr_seat_touch_get_point(seat_, candidate)) {
            return candidate;
        }
    } while (next_touch_id_ != first);
    return -1;
}

void WlrServer::HandleTouchDown(WlrTouchBinding *binding, const wlr_touch_down_event &event)
{
    if (!seat_ || !scene_ || !std::isfinite(event.x) || !std::isfinite(event.y) ||
        binding->contacts.contains(event.touch_id)) {
        return;
    }

    double lx{}, ly{}, sx{}, sy{};
    wlr_cursor_absolute_to_layout_coords(cursor_, &binding->touch->base, event.x, event.y, &lx,
                                         &ly);
    wlr_surface *surface = nullptr;
    for (auto *tree : {chrome_tree_, windows_tree_, background_tree_}) {
        auto *node = tree ? wlr_scene_node_at(&tree->node, lx, ly, &sx, &sy) : nullptr;
        if (node && node->type == WLR_SCENE_NODE_BUFFER) {
            auto *scene_surface =
                wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
            surface = scene_surface ? scene_surface->surface : nullptr;
        }
        if (surface) {
            break;
        }
    }
    if (!surface || !wlr_surface_accepts_touch(seat_, surface)) {
        return;
    }

    if (auto *owner = XdgOwner(surface)) {
        FocusXdgView(owner);
    }
    if (!SurfacePosition(scene_, surface, lx, ly, sx, sy)) {
        return;
    }

    const auto contact = AllocateTouchId();
    if (contact < 0) {
        return;
    }
    const auto serial =
        wlr_seat_touch_notify_down(seat_, surface, event.time_msec, contact, sx, sy);
    if (serial) {
        binding->contacts.emplace(event.touch_id, contact);
        RecordLayoutInput(surface, {contracts::LayoutInputKind::Touch, serial, contact});
    }
}

void WlrServer::HandleTouchMotion(WlrTouchBinding *binding, const wlr_touch_motion_event &event)
{
    const auto it = binding->contacts.find(event.touch_id);
    if (it == binding->contacts.end() || !std::isfinite(event.x) || !std::isfinite(event.y)) {
        return;
    }
    auto *point = wlr_seat_touch_get_point(seat_, it->second);
    if (!point) {
        binding->contacts.erase(it);
        return;
    }

    double lx{}, ly{}, sx{}, sy{};
    wlr_cursor_absolute_to_layout_coords(cursor_, &binding->touch->base, event.x, event.y, &lx,
                                         &ly);
    if (!SurfacePosition(scene_, point->surface, lx, ly, sx, sy)) {
        CancelTouchClient(point->client);
        return;
    }

    // Every motion remains relative to the surface that received this contact's down.
    wlr_seat_touch_notify_motion(seat_, event.time_msec, it->second, sx, sy);
}

void WlrServer::HandleTouchUp(WlrTouchBinding *binding, std::uint32_t time_msec,
                              std::int32_t contact)
{
    const auto it = binding->contacts.find(contact);
    if (it == binding->contacts.end()) {
        return;
    }

    const auto seat_contact = it->second;
    binding->contacts.erase(it);
    layout_controls_.ReleaseInput(contracts::LayoutInputKind::Touch, seat_contact,
                                  launch::MonotonicNs());
    wlr_seat_touch_notify_up(seat_, time_msec, seat_contact);
}

void WlrServer::CancelTouchClient(wlr_seat_client *client)
{
    // wl_touch.cancel covers the seat's whole logical touch device for this client,
    // including contacts originating on a different physical touchscreen.
    for (const auto &binding : touches_) {
        for (const auto &[device_contact, seat_contact] : binding->contacts) {
            auto *point = wlr_seat_touch_get_point(seat_, seat_contact);
            if (!point || point->client == client) {
                layout_controls_.CancelInput(contracts::LayoutInputKind::Touch, seat_contact);
            }
        }
        std::erase_if(binding->contacts, [this, client](const auto &entry) {
            auto *point = wlr_seat_touch_get_point(seat_, entry.second);
            return !point || point->client == client;
        });
    }
    wlr_seat_touch_notify_cancel(seat_, client);
}

void WlrServer::HandleTouchCancel(WlrTouchBinding *binding, std::int32_t contact)
{
    const auto it = binding->contacts.find(contact);
    if (it == binding->contacts.end()) {
        return;
    }
    auto *point = wlr_seat_touch_get_point(seat_, it->second);
    if (!point) {
        binding->contacts.erase(it);
        return;
    }

    CancelTouchClient(point->client);
}

void WlrServer::CancelTouchDevice(WlrTouchBinding *binding)
{
    while (!binding->contacts.empty()) {
        const auto it = binding->contacts.begin();
        auto *point = wlr_seat_touch_get_point(seat_, it->second);
        if (point) {
            CancelTouchClient(point->client);
        } else {
            binding->contacts.erase(it);
        }
    }
}

void WlrServer::CancelTouchesForSurface(wlr_surface *surface)
{
    if (!seat_) {
        return;
    }
    for (const auto &binding : touches_) {
        bool again = true;
        while (again) {
            again = false;
            for (const auto &[device_contact, seat_contact] : binding->contacts) {
                auto *point = wlr_seat_touch_get_point(seat_, seat_contact);
                if (point && point->surface &&
                    (point->surface == surface ||
                     wlr_surface_get_root_surface(point->surface) == surface)) {
                    CancelTouchClient(point->client);
                    again = true;
                    break;
                }
            }
        }
    }
}

void WlrServer::HandleTouchFrame()
{
    wlr_seat_touch_notify_frame(seat_);
}

void WlrServer::HandleTouchDestroy(WlrTouchBinding *binding)
{
    CancelTouchDevice(binding);
    wlr_cursor_detach_input_device(cursor_, &binding->touch->base);
    std::erase_if(touches_,
                  [binding](const auto &candidate) { return candidate.get() == binding; });
    if (touches_.empty()) {
        wlr_seat_set_capabilities(seat_, seat_->capabilities & ~WL_SEAT_CAPABILITY_TOUCH);
    }
}

} // namespace prism::wm
