#include "wlr_server_internal.hpp"

namespace prism::wm {
void WlrServer::HandleNewInput(struct wlr_input_device *device)
{
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        auto *wlr_kbd = wlr_keyboard_from_input_device(device);
        struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *keymap =
            xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(wlr_kbd, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);

        wlr_keyboard_set_repeat_info(wlr_kbd, 25, 600);
        wlr_seat_set_keyboard(seat_, wlr_kbd);
        auto binding = std::make_unique<WlrKeyboardBinding>();
        binding->server = this;
        binding->keyboard = wlr_kbd;
        binding->key.notify = handle_keyboard_key;
        binding->modifiers.notify = handle_keyboard_modifiers;
        binding->destroy.notify = handle_keyboard_destroy;
        wl_signal_add(&wlr_kbd->events.key, &binding->key);
        wl_signal_add(&wlr_kbd->events.modifiers, &binding->modifiers);
        wl_signal_add(&device->events.destroy, &binding->destroy);
        keyboards_.push_back(std::move(binding));
        if (focused_xdg_view_) {
            FocusXdgView(focused_xdg_view_);
        }
        PRISM_LOG_INFO("WLR-INPUT", "Keyboard attached: %s", device->name);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER || device->type == WLR_INPUT_DEVICE_TOUCH) {
        wlr_cursor_attach_input_device(cursor_, device);
        PRISM_LOG_INFO("WLR-INPUT", "Pointer/Touchpad attached: %s", device->name);
    }
}

void WlrServer::HandleKeyboardKey(WlrKeyboardBinding *binding, void *data)
{
    auto *event = static_cast<wlr_keyboard_key_event *>(data);
    wlr_seat_set_keyboard(seat_, binding->keyboard);
    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED &&
        event->keycode < binding->consumed_keys.size()) {
        handled = binding->consumed_keys[event->keycode];
        binding->consumed_keys[event->keycode] = false;
    }
    const auto mods = wlr_keyboard_get_modifiers(binding->keyboard);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && (mods & WLR_MODIFIER_LOGO)) {
        const xkb_keysym_t *symbols{};
        const auto layout =
            xkb_state_key_get_layout(binding->keyboard->xkb_state, event->keycode + 8);
        const int count = xkb_keymap_key_get_syms_by_level(binding->keyboard->keymap,
                                                           event->keycode + 8, layout, 0, &symbols);
        for (int i = 0; i < count && !handled; ++i) {
            tree::Direction direction{};
            bool directional = true;
            switch (symbols[i]) {
            case XKB_KEY_Left:
            case XKB_KEY_h:
                direction = tree::Direction::Left;
                break;
            case XKB_KEY_Right:
            case XKB_KEY_l:
                direction = tree::Direction::Right;
                break;
            case XKB_KEY_Up:
            case XKB_KEY_k:
                direction = tree::Direction::Up;
                break;
            case XKB_KEY_Down:
            case XKB_KEY_j:
                direction = tree::Direction::Down;
                break;
            default:
                directional = false;
            }
            if (directional) {
                if (mods & WLR_MODIFIER_SHIFT) {
                    compositor_->SwapFocusDirection(direction);
                } else if (!(focused_xdg_view_ && focused_xdg_view_->fullscreen)) {
                    compositor_->MoveFocus(direction);
                }
                handled = true;
            } else if (symbols[i] == XKB_KEY_v) {
                compositor_->SetTreeLayout(tree::LayoutMode::SplitVertical);
                handled = true;
            } else if (symbols[i] == XKB_KEY_b) {
                compositor_->SetTreeLayout(tree::LayoutMode::SplitHorizontal);
                handled = true;
            } else if (symbols[i] == XKB_KEY_f) {
                if (focused_xdg_view_) {
                    SetXdgFullscreen(focused_xdg_view_, !focused_xdg_view_->fullscreen);
                }
                handled = true;
            } else if ((mods & WLR_MODIFIER_SHIFT) &&
                       (symbols[i] == XKB_KEY_q || symbols[i] == XKB_KEY_Q)) {
                CloseFocusedXdgView();
                handled = true;
            } else if (symbols[i] >= XKB_KEY_1 && symbols[i] <= XKB_KEY_9) {
                const auto name = std::to_string(symbols[i] - XKB_KEY_1 + 1);
                if (mods & WLR_MODIFIER_SHIFT) {
                    compositor_->GetTreeEngine().MoveWindowToWorkspace(
                        compositor_->GetTreeEngine().GetFocusedWindow(), name);
                    compositor_->SynchronizeFocus();
                } else {
                    compositor_->SwitchWorkspace(name);
                }
                handled = true;
            }
        }
        if (handled) {
            if (event->keycode < binding->consumed_keys.size()) {
                binding->consumed_keys[event->keycode] = true;
            }
            ArrangeXdgViews();
            SynchronizeXdgFocus();
        }
    }
    if (!handled) {
        wlr_seat_keyboard_notify_key(seat_, event->time_msec, event->keycode, event->state);
    }
}

void WlrServer::HandleKeyboardModifiers(WlrKeyboardBinding *binding)
{
    wlr_seat_set_keyboard(seat_, binding->keyboard);
    wlr_seat_keyboard_notify_modifiers(seat_, &binding->keyboard->modifiers);
}

void WlrServer::HandleKeyboardDestroy(WlrKeyboardBinding *binding)
{
    if (wlr_seat_get_keyboard(seat_) == binding->keyboard) {
        wlr_seat_set_keyboard(seat_, nullptr);
    }
    auto it = std::find_if(keyboards_.begin(), keyboards_.end(),
                           [binding](const auto &item) { return item.get() == binding; });
    if (it != keyboards_.end()) {
        keyboards_.erase(it);
    }
}

void WlrServer::CloseFocusedXdgView()
{
    if (focused_xdg_view_ && focused_xdg_view_->toplevel) {
        wlr_xdg_toplevel_send_close(focused_xdg_view_->toplevel);
    }
}

void WlrServer::HandleCursorMotion(uint32_t time_msec, double dx, double dy)
{
    ++pointer_events_;
    const auto age = static_cast<std::uint32_t>(core::CurrentTimeNs() / 1000000ULL) - time_msec;
    if (age < 60000) {
        pointer_event_age_.Record(age);
    }
    wlr_cursor_move(cursor_, nullptr, dx, dy);
    UpdateXdgPointerFocus(time_msec);
    if (compositor_) {
        compositor_->OnPointerMotion(static_cast<float>(cursor_->x), static_cast<float>(cursor_->y),
                                     static_cast<float>(dx), static_cast<float>(dy));
    }
    if (drag_manager_ && drag_manager_->IsDragging() && compositor_) {
        drag_manager_->UpdateDrag(static_cast<float>(cursor_->x), static_cast<float>(cursor_->y),
                                  compositor_->GetWindows());
    }
    // wlroots handles hardware cursor plane updates and software cursor damage.
    // Client hover changes submit their own damage; ordinary motion does not
    // force every output to redraw or reset an unchanged cursor image.
}

void WlrServer::HandleCursorMotionAbsolute(uint32_t time_msec, double x, double y)
{
    ++pointer_events_;
    const auto age = static_cast<std::uint32_t>(core::CurrentTimeNs() / 1000000ULL) - time_msec;
    if (age < 60000) {
        pointer_event_age_.Record(age);
    }
    wlr_cursor_warp_absolute(cursor_, nullptr, x, y);
    UpdateXdgPointerFocus(time_msec);
    if (compositor_) {
        compositor_->OnPointerMotion(static_cast<float>(cursor_->x),
                                     static_cast<float>(cursor_->y));
    }
    if (drag_manager_ && drag_manager_->IsDragging() && compositor_) {
        drag_manager_->UpdateDrag(static_cast<float>(cursor_->x), static_cast<float>(cursor_->y),
                                  compositor_->GetWindows());
    }
}

void WlrServer::HandleCursorButton(uint32_t time_msec, uint32_t button, uint32_t state)
{
    WlrXdgView *pointed = nullptr;
    if (seat_->pointer_state.focused_surface) {
        auto *surface = wlr_surface_get_root_surface(seat_->pointer_state.focused_surface);
        auto *top = wlr_xdg_toplevel_try_from_wlr_surface(surface);
        for (auto &view : xdg_views_) {
            if (view->toplevel == top && view->visible && !view->shell_role) {
                pointed = view.get();
            }
        }
    }
    auto *keyboard = wlr_seat_get_keyboard(seat_);
    if (button == 272 && state == WLR_BUTTON_PRESSED && pointed && !pointed->fullscreen &&
        keyboard && (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO)) {
        FocusXdgView(pointed);
        dragged_xdg_view_ = pointed;
        return;
    }
    if (button == 272 && state == WLR_BUTTON_RELEASED && dragged_xdg_view_) {
        auto *source = dragged_xdg_view_;
        dragged_xdg_view_ = nullptr;
        if (pointed && pointed != source && !pointed->fullscreen) {
            auto &tree = compositor_->GetTreeEngine();
            auto target = tree.FindViewForWindow(pointed->managed);
            auto source_node = tree.FindViewForWindow(source->managed);
            const double dx = (cursor_->x - pointed->x) / std::max(1, pointed->width) - 0.5;
            const double dy = (cursor_->y - pointed->y) / std::max(1, pointed->height) - 0.5;
            if (std::abs(dx) < 0.18 && std::abs(dy) < 0.18) {
                tree.SwapNodes(source_node, target);
            } else {
                const auto direction =
                    std::abs(dx) > std::abs(dy)
                        ? (dx < 0 ? tree::Direction::Left : tree::Direction::Right)
                        : (dy < 0 ? tree::Direction::Up : tree::Direction::Down);
                if (tree.RemoveWindow(source->managed)) {
                    tree.InsertWindow(source->managed, direction, target);
                }
            }
            ArrangeXdgViews();
            SynchronizeXdgFocus();
        }
        return;
    }
    if (state == WLR_BUTTON_PRESSED && seat_->pointer_state.focused_surface) {
        auto *top = wlr_xdg_toplevel_try_from_wlr_surface(seat_->pointer_state.focused_surface);
        for (auto &view : xdg_views_) {
            if (view->toplevel == top) {
                FocusXdgView(view.get());
                break;
            }
        }
    }
    wlr_seat_pointer_notify_button(seat_, time_msec, button,
                                   static_cast<wl_pointer_button_state>(state));
    wlr_seat_pointer_notify_frame(seat_);
    if (compositor_) {
        compositor_->OnPointerButton(button, state == WLR_BUTTON_PRESSED);
    }

    // Tiling Window Header & Drag-to-Split interaction (Left Click: 1 or 272)
    if (button == 1 || button == 272) {
        if (state == WLR_BUTTON_PRESSED) {
            if (compositor_) {
                const auto &windows = compositor_->GetWindows();
                for (size_t i = 0; i < windows.size(); ++i) {
                    auto &win = windows[i];
                    if (win->IsNative()) {
                        continue;
                    }
                    auto b = win->GetBounds();
                    if (cursor_->x >= b.x && cursor_->x <= b.x + b.width && cursor_->y >= b.y &&
                        cursor_->y <= b.y + b.height) {

                        compositor_->SetFocusedWindowIndex(static_cast<int>(i));

                        if (auto *dec = win->GetDecorator()) {
                            float lx = static_cast<float>(cursor_->x - b.x);
                            float ly = static_cast<float>(cursor_->y - b.y);
                            auto action = dec->HitTestHeader(lx, ly);
                            if (action == decoration::HeaderAction::TitlebarDrag) {
                                if (drag_manager_) {
                                    drag_manager_->BeginDrag(win, static_cast<float>(cursor_->x),
                                                             static_cast<float>(cursor_->y));
                                }
                            } else if (action == decoration::HeaderAction::Close) {
                                PRISM_LOG_INFO("WM-CHROME", "Clicked Close on tile '%s'",
                                               win->GetTitle().c_str());
                            } else if (action == decoration::HeaderAction::ToggleSplit) {
                                PRISM_LOG_INFO("WM-CHROME", "Clicked ToggleSplit on tile '%s'",
                                               win->GetTitle().c_str());
                            } else if (action == decoration::HeaderAction::ToggleFold) {
                                dec->ToggleFold();
                                PRISM_LOG_INFO("WM-CHROME",
                                               "Clicked ToggleFold on tile '%s' (folded=%d)",
                                               win->GetTitle().c_str(), dec->IsFolded());
                            } else if (action == decoration::HeaderAction::ToggleMonocle) {
                                core::Rect screen{0.0f, 30.0f, 1920.0f, 1050.0f};
                                if (!outputs_.empty() && outputs_[0]->wlr_output) {
                                    screen = core::Rect{
                                        0.0f, 30.0f,
                                        static_cast<float>(outputs_[0]->wlr_output->width),
                                        static_cast<float>(outputs_[0]->wlr_output->height - 30)};
                                }
                                dec->ToggleFullscreen(screen);
                                PRISM_LOG_INFO("WM-CHROME",
                                               "Clicked ToggleMonocle on tile '%s' (fullscreen=%d)",
                                               win->GetTitle().c_str(), dec->IsFullscreen());
                            }
                        }
                        break;
                    }
                }
            }
        } else {
            if (drag_manager_ && drag_manager_->IsDragging()) {
                auto res = drag_manager_->EndDrag();
                if (res.executed && res.source_window && res.target_window &&
                    res.source_window != res.target_window && compositor_) {
                    if (res.quadrant == decoration::DropQuadrant::Swap) {
                        auto src_node =
                            compositor_->GetTreeEngine().FindViewForWindow(res.source_window);
                        auto tgt_node =
                            compositor_->GetTreeEngine().FindViewForWindow(res.target_window);
                        if (src_node && tgt_node) {
                            compositor_->GetTreeEngine().SwapNodes(src_node, tgt_node);
                        }
                    } else {
                        tree::Direction dir = tree::Direction::Right;
                        if (res.quadrant == decoration::DropQuadrant::LeftSplit) {
                            dir = tree::Direction::Left;
                        } else if (res.quadrant == decoration::DropQuadrant::RightSplit) {
                            dir = tree::Direction::Right;
                        } else if (res.quadrant == decoration::DropQuadrant::TopSplit) {
                            dir = tree::Direction::Up;
                        } else if (res.quadrant == decoration::DropQuadrant::BottomSplit) {
                            dir = tree::Direction::Down;
                        }

                        auto tgt_node =
                            compositor_->GetTreeEngine().FindViewForWindow(res.target_window);
                        compositor_->GetTreeEngine().RemoveWindow(res.source_window);
                        compositor_->GetTreeEngine().InsertWindow(res.source_window, dir, tgt_node);
                    }
                    PRISM_LOG_INFO("WM-TILING", "Committed Drag-to-Split between [%s] and [%s]",
                                   res.source_window->GetTitle().c_str(),
                                   res.target_window->GetTitle().c_str());
                }
            }
        }
    }

    // Focus/topology changes and client reactions carry their own frame demand.
}

void WlrServer::HandleCursorAxis(uint32_t time_msec, int axis, double value)
{
    wlr_seat_pointer_notify_axis(seat_, time_msec, static_cast<wl_pointer_axis>(axis), value, 0,
                                 WL_POINTER_AXIS_SOURCE_FINGER,
                                 WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    wlr_seat_pointer_notify_frame(seat_);
}

void WlrServer::UpdateXdgPointerFocus(uint32_t time_msec)
{
    if (!windows_tree_ || !seat_ || !cursor_) {
        return;
    }
    double sx = 0.0;
    double sy = 0.0;
    struct wlr_surface *surface = nullptr;
    for (auto *tree : {chrome_tree_, windows_tree_, background_tree_}) {
        auto *node = wlr_scene_node_at(&tree->node, cursor_->x, cursor_->y, &sx, &sy);
        if (node && node->type == WLR_SCENE_NODE_BUFFER) {
            auto *scene_surface =
                wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
            if (scene_surface) {
                surface = scene_surface->surface;
            }
        }
        if (surface) {
            break;
        }
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat_, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat_, time_msec, sx, sy);
    } else {
        wlr_seat_pointer_notify_clear_focus(seat_);
    }
    wlr_seat_pointer_notify_frame(seat_);
}

} // namespace prism::wm
