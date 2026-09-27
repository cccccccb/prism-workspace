#include "wlr_server_internal.hpp"

namespace prism::wm {
WlrXdgView::WlrXdgView()
{
    for (auto *listener : {&map, &commit, &unmap, &destroy, &request_maximize, &request_fullscreen,
                           &set_title, &set_app_id}) {
        wl_list_init(&listener->link);
    }
}

WlrXdgView::~WlrXdgView()
{
    for (auto *listener : {&map, &commit, &unmap, &destroy, &request_maximize, &request_fullscreen,
                           &set_title, &set_app_id}) {
        wl_list_remove(&listener->link);
    }
}

void handle_output_frame(struct wl_listener *listener, void *data)
{
    auto *output = WlContainerOf<WlrOutput>(listener, offsetof(WlrOutput, frame));
    output->server->HandleOutputFrame(output);
}

void handle_output_present(struct wl_listener *listener, void *data)
{
    auto *output = WlContainerOf<WlrOutput>(listener, offsetof(WlrOutput, present));
    const auto *event = static_cast<wlr_output_event_present *>(data);
    if (!event->presented) {
        ++output->discarded_count;
        return;
    }
    ++output->presented_count;
    if (!event->when) {
        return;
    }
    const std::uint64_t now =
        static_cast<std::uint64_t>(event->when->tv_sec) * 1000000000ULL + event->when->tv_nsec;
    if (output->last_present_ns && now > output->last_present_ns) {
        output->present_intervals.Record((now - output->last_present_ns) / 1e6);
    }
    output->last_present_ns = now;
}

void handle_output_commit(wl_listener *listener, void *data)
{
    auto *output = WlContainerOf<WlrOutput>(listener, offsetof(WlrOutput, committed));
    output->server->HandleOutputCommit(data);
}

void handle_output_needs_frame(wl_listener *listener, void *)
{
    auto *output = WlContainerOf<WlrOutput>(listener, offsetof(WlrOutput, needs_frame));
    output->server->HandleOutputNeedsFrame();
}

void handle_output_damage(wl_listener *listener, void *)
{
    auto *output = WlContainerOf<WlrOutput>(listener, offsetof(WlrOutput, damage));
    output->server->HandleOutputDamage();
}

void handle_server_new_surface(wl_listener *listener, void *data)
{
    auto *signals =
        WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, new_surface));
    signals->server->HandleNewSurface(static_cast<wlr_surface *>(data));
}

void handle_output_destroy(struct wl_listener *listener, void *data)
{
    auto *output = WlContainerOf<WlrOutput>(listener, offsetof(WlrOutput, destroy));
    if (output && output->server) {
        output->server->RemoveOutput(output);
    }
}

void handle_server_new_xdg_surface(struct wl_listener *listener, void *data)
{
    auto *sig =
        WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, new_xdg_toplevel));
    sig->server->HandleNewXdgToplevel(static_cast<struct wlr_xdg_toplevel *>(data));
}

void handle_xdg_map(struct wl_listener *listener, void *)
{
    auto *view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, map));
    view->server->HandleXdgMap(view);
}

void handle_xdg_unmap(struct wl_listener *listener, void *)
{
    auto *view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, unmap));
    view->server->HandleXdgUnmap(view);
}

void handle_xdg_destroy(struct wl_listener *listener, void *)
{
    auto *view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, destroy));
    view->server->HandleXdgDestroy(view);
}

void handle_xdg_maximize(struct wl_listener *listener, void *)
{
    auto *view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, request_maximize));
    view->server->HandleXdgMaximize(view);
}

void handle_xdg_fullscreen(struct wl_listener *listener, void *)
{
    auto *view = WlContainerOf<WlrXdgView>(listener, offsetof(WlrXdgView, request_fullscreen));
    view->server->HandleXdgMaximize(view);
}

void handle_keyboard_key(struct wl_listener *listener, void *data)
{
    auto *binding = WlContainerOf<WlrKeyboardBinding>(listener, offsetof(WlrKeyboardBinding, key));
    binding->server->HandleKeyboardKey(binding, data);
}

void handle_keyboard_modifiers(struct wl_listener *listener, void *)
{
    auto *binding =
        WlContainerOf<WlrKeyboardBinding>(listener, offsetof(WlrKeyboardBinding, modifiers));
    binding->server->HandleKeyboardModifiers(binding);
}

void handle_keyboard_destroy(struct wl_listener *listener, void *)
{
    auto *binding =
        WlContainerOf<WlrKeyboardBinding>(listener, offsetof(WlrKeyboardBinding, destroy));
    binding->server->HandleKeyboardDestroy(binding);
}

WlrOutput::WlrOutput(struct wlr_output *out, WlrServer *s) : wlr_output(out), server(s)
{
    frame.notify = handle_output_frame;
    wl_signal_add(&wlr_output->events.frame, &frame);

    present.notify = handle_output_present;
    wl_signal_add(&wlr_output->events.present, &present);
    committed.notify = handle_output_commit;
    wl_signal_add(&wlr_output->events.commit, &committed);
    needs_frame.notify = handle_output_needs_frame;
    wl_signal_add(&wlr_output->events.needs_frame, &needs_frame);
    damage.notify = handle_output_damage;
    wl_signal_add(&wlr_output->events.damage, &damage);
    destroy.notify = handle_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &destroy);
}

WlrOutput::~WlrOutput()
{
    if (scene_output) {
        wlr_scene_output_destroy(scene_output);
        scene_output = nullptr;
    }
    wl_list_remove(&frame.link);
    wl_list_remove(&present.link);
    wl_list_remove(&committed.link);
    wl_list_remove(&needs_frame.link);
    wl_list_remove(&damage.link);
    wl_list_remove(&destroy.link);
}

// Server listeners
void handle_server_new_output(struct wl_listener *listener, void *data)
{
    auto *sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, new_output));
    sig->server->HandleNewOutput(static_cast<struct wlr_output *>(data));
}

void handle_server_new_input(struct wl_listener *listener, void *data)
{
    auto *sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, new_input));
    sig->server->HandleNewInput(static_cast<struct wlr_input_device *>(data));
}

void handle_cursor_motion(struct wl_listener *listener, void *data)
{
    auto *sig =
        WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, cursor_motion));
    auto *event = static_cast<struct wlr_pointer_motion_event *>(data);
    sig->server->HandleCursorMotion(event->time_msec, event->delta_x, event->delta_y);
}

void handle_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
    auto *sig = WlContainerOf<WlrServerSignals>(listener,
                                                offsetof(WlrServerSignals, cursor_motion_absolute));
    auto *event = static_cast<struct wlr_pointer_motion_absolute_event *>(data);
    sig->server->HandleCursorMotionAbsolute(event->time_msec, event->x, event->y);
}

void handle_cursor_button(struct wl_listener *listener, void *data)
{
    auto *sig =
        WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, cursor_button));
    auto *event = static_cast<struct wlr_pointer_button_event *>(data);
    sig->server->HandleCursorButton(event->time_msec, event->button, event->state);
}

void handle_cursor_axis(struct wl_listener *listener, void *data)
{
    auto *sig = WlContainerOf<WlrServerSignals>(listener, offsetof(WlrServerSignals, cursor_axis));
    auto *event = static_cast<struct wlr_pointer_axis_event *>(data);
    sig->server->HandleCursorAxis(event->time_msec, event->orientation, event->delta);
}

} // namespace prism::wm
