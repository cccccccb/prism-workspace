// A protocol fixture: popup buffers and the compositor live only in this test.
#include "presentation-time-client-protocol.h"
#include "prism-surface-effects-server.h"
#include "prism/platform/wayland_popup.hpp"
#include "prism/platform/wayland_window.hpp"
#include "xdg-shell-protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>
#include <wayland-server.h>

namespace {
using namespace std::chrono_literals;
using namespace prism;
using platform::WaylandPopup;
using platform::WaylandPopupBufferLayout;
using platform::WaylandPopupCloseReason;
using platform::WaylandWindow;

struct PositionerState {
    contracts::PopupPositionerRect anchor;
    int width{}, height{}, offset_x{}, offset_y{};
    std::uint32_t anchor_edge{}, gravity{}, constraints{};
    unsigned size_calls{}, anchor_rect_calls{}, anchor_calls{}, gravity_calls{}, constraint_calls{},
        offset_calls{}, extension_calls{};

    bool operator==(const PositionerState &) const = default;
};

struct PopupState {
    PositionerState positioner;
    bool correct_parent{};
    unsigned empty_commits{}, buffer_commits{}, attachments{}, acks{};
    unsigned popup_destroy_order{}, xdg_destroy_order{}, surface_destroy_order{};
    std::uint32_t last_ack{};
    int buffer_width{}, buffer_height{};
    unsigned geometry_requests{}, damage_requests{}, geometry_order{}, attachment_order{},
        damage_order{}, commit_order{};
    contracts::LogicalRect window_geometry, damage;
    unsigned frame_requests{}, feedback_requests{}, input_requests{};
    unsigned effect_creates{}, effect_clears{}, effect_rectangles{}, effect_contours{},
        effect_destroy_order{}, effect_request_order{}, effect_commits{};
    std::vector<contracts::SurfaceEffectRegion> pending_effects, current_effects;
    std::vector<std::uint8_t> last_effect_payload;

    bool operator==(const PopupState &) const = default;
};

struct ServerState {
    unsigned clients{}, seats{}, pointers{}, keyboards{}, touches{}, toplevels{}, positioners{},
        positioner_destroys{}, surfaces{}, live_surfaces{}, grabs{}, repositions{}, destroy_order{},
        parent_buffer_commits{}, protocol_order{};
    std::vector<PopupState> popups;

    bool operator==(const ServerState &) const = default;
};

class Server {
    enum class Role { None, Parent, Popup };

    struct Surface {
        Server *server{};
        wl_resource *native{}, *xdg{}, *toplevel{}, *popup{}, *pending_buffer{}, *effect{};
        Role role{Role::None};
        std::size_t popup_index{};
        bool configured{}, attached{};
        std::vector<wl_resource *> frames;
    };

    struct Feedback {
        Surface *surface{};
        wl_resource *resource{};
        bool committed{};
    };

    struct Positioner {
        Server *server{};
        PositionerState state;
    };

    enum class OperationKind {
        Inspect,
        Configure,
        ParentConfigure,
        Done,
        DismissWithLateConfigure,
        Input,
        FocusChild,
        ReleaseFrames,
        ReleaseFeedback,
        HoldFrames,
        RemoveDevices,
        Fault
    };

    struct Operation {
        OperationKind kind{OperationKind::Inspect};
        std::size_t popup{};
        contracts::LogicalRect bounds;
        bool child{};
        unsigned feedback_index{};
        std::promise<ServerState> finished;
    };

public:
    explicit Server(const std::string &socket, bool presentation = false,
                    unsigned effects_version = 0, bool popup_backdrop = true, bool contour = true,
                    bool backdrop = true)
        : effects_version_(effects_version), popup_backdrop_(popup_backdrop), contour_(contour),
          backdrop_(backdrop)
    {
        display_ = wl_display_create();
        assert(display_ && wl_display_init_shm(display_) == 0);
        assert(wl_global_create(display_, &wl_compositor_interface, 4, this, BindCompositor));
        assert(wl_global_create(display_, &xdg_wm_base_interface, 3, this, BindShell));
        assert(wl_global_create(display_, &wl_seat_interface, 5, this, BindSeat));
        if (presentation) {
            assert(
                wl_global_create(display_, &wp_presentation_interface, 1, this, BindPresentation));
        }
        if (effects_version) {
            assert(wl_global_create(display_, &prism_surface_effect_manager_v1_interface,
                                    effects_version, this, BindEffects));
        }
        assert(wl_display_add_socket(display_, socket.c_str()) == 0);
        thread_ = std::thread(&Server::Run, this);
    }

    ~Server()
    {
        stop_ = true;
        thread_.join();
        wl_display_destroy(display_);
    }

    ServerState Inspect()
    {
        return Submit(std::make_shared<Operation>());
    }

    ServerState Configure(std::size_t popup, contracts::LogicalRect bounds)
    {
        auto operation = std::make_shared<Operation>();
        operation->kind = OperationKind::Configure;
        operation->popup = popup;
        operation->bounds = bounds;
        return Submit(std::move(operation));
    }

    void ResizeParent(int width, int height)
    {
        auto operation = std::make_shared<Operation>();
        operation->kind = OperationKind::ParentConfigure;
        operation->bounds = {0, 0, double(width), double(height)};
        Submit(std::move(operation));
    }

    void Done(std::size_t popup, bool late_configure = false)
    {
        auto operation = std::make_shared<Operation>();
        operation->kind =
            late_configure ? OperationKind::DismissWithLateConfigure : OperationKind::Done;
        operation->popup = popup;
        Submit(std::move(operation));
    }

    void Input(bool child, std::size_t popup = 0)
    {
        auto operation = std::make_shared<Operation>();
        operation->kind = OperationKind::Input;
        operation->child = child;
        operation->popup = popup;
        Submit(std::move(operation));
    }

    void Control(OperationKind kind, std::size_t popup, bool child = false,
                 unsigned feedback_index = 0)
    {
        auto operation = std::make_shared<Operation>();
        operation->kind = kind;
        operation->popup = popup;
        operation->child = child;
        operation->feedback_index = feedback_index;
        Submit(std::move(operation));
    }

    void FocusChild(std::size_t popup)
    {
        Control(OperationKind::FocusChild, popup);
    }

    void HoldFrames(bool enabled)
    {
        Control(OperationKind::HoldFrames, 0, enabled);
    }

    void ReleaseFrames(std::size_t popup)
    {
        Control(OperationKind::ReleaseFrames, popup);
    }

    void ReleaseFeedback(std::size_t popup, unsigned index, bool discarded = false)
    {
        Control(OperationKind::ReleaseFeedback, popup, discarded, index);
    }

    void RemoveDevices()
    {
        Control(OperationKind::RemoveDevices, 0);
    }

    void Fault(std::size_t popup)
    {
        auto operation = std::make_shared<Operation>();
        operation->kind = OperationKind::Fault;
        operation->popup = popup;
        Submit(std::move(operation));
    }

private:
    ServerState Submit(std::shared_ptr<Operation> operation)
    {
        auto finished = operation->finished.get_future();
        {
            std::lock_guard lock(mutex_);
            operations_.push_back(std::move(operation));
        }
        assert(finished.wait_for(3s) == std::future_status::ready);
        return finished.get();
    }

    void Run()
    {
        auto *loop = wl_display_get_event_loop(display_);
        while (!stop_) {
            assert(wl_event_loop_dispatch(loop, 2) >= 0);
            std::vector<std::shared_ptr<Operation>> operations;
            {
                std::lock_guard lock(mutex_);
                operations.swap(operations_);
            }
            for (const auto &operation : operations) {
                Process(*operation);
                operation->finished.set_value(state_);
            }
            wl_display_flush_clients(display_);
        }
        wl_display_destroy_clients(display_);
    }

    Surface &FindPopup(std::size_t index)
    {
        const auto found = std::find_if(surfaces_.begin(), surfaces_.end(), [index](Surface *item) {
            return item->role == Role::Popup && item->popup_index == index && item->popup;
        });
        assert(found != surfaces_.end());
        return **found;
    }

    std::uint32_t SendConfigure(Surface &surface, contracts::LogicalRect bounds)
    {
        xdg_popup_send_configure(surface.popup, static_cast<int>(bounds.x),
                                 static_cast<int>(bounds.y), static_cast<int>(bounds.width),
                                 static_cast<int>(bounds.height));
        const auto serial = wl_display_next_serial(display_);
        xdg_surface_send_configure(surface.xdg, serial);
        surface.configured = true;
        return serial;
    }

    void Process(const Operation &operation)
    {
        switch (operation.kind) {
        case OperationKind::Inspect:
            break;
        case OperationKind::Configure:
            SendConfigure(FindPopup(operation.popup), operation.bounds);
            break;
        case OperationKind::ParentConfigure: {
            assert(parent_);
            wl_array states;
            wl_array_init(&states);
            xdg_toplevel_send_configure(parent_->toplevel, static_cast<int>(operation.bounds.width),
                                        static_cast<int>(operation.bounds.height), &states);
            wl_array_release(&states);
            xdg_surface_send_configure(parent_->xdg, wl_display_next_serial(display_));
            break;
        }
        case OperationKind::Done:
            xdg_popup_send_popup_done(FindPopup(operation.popup).popup);
            break;
        case OperationKind::DismissWithLateConfigure: {
            auto &surface = FindPopup(operation.popup);
            xdg_popup_send_popup_done(surface.popup);
            SendConfigure(surface, {999, 999, 17, 19});
            break;
        }
        case OperationKind::Input:
            SendInput(operation.child ? FindPopup(operation.popup) : *parent_);
            break;
        case OperationKind::FocusChild: {
            auto &surface = FindPopup(operation.popup);
            wl_keyboard_send_leave(keyboard_, wl_display_next_serial(display_), parent_->native);
            wl_array keys{};
            wl_keyboard_send_enter(keyboard_, wl_display_next_serial(display_), surface.native,
                                   &keys);
            wl_pointer_send_enter(pointer_, wl_display_next_serial(display_), surface.native,
                                  wl_fixed_from_int(18), wl_fixed_from_int(20));
            break;
        }
        case OperationKind::HoldFrames:
            hold_child_frames_ = operation.child;
            break;
        case OperationKind::ReleaseFrames:
            FinishFrames(FindPopup(operation.popup));
            break;
        case OperationKind::ReleaseFeedback: {
            auto &surface = FindPopup(operation.popup);
            unsigned index{};
            const auto records = feedbacks_;
            for (auto *feedback : records) {
                if (feedback->surface != &surface || !feedback->committed ||
                    index++ != operation.feedback_index) {
                    continue;
                }
                if (operation.child) {
                    wl_resource_post_event(feedback->resource, 2);
                } else {
                    wl_resource_post_event(feedback->resource, 1, 0u, 1u, 0u, 16666667u, 0u, 1u,
                                           0u);
                }
                wl_resource_destroy(feedback->resource);
                break;
            }
            break;
        }
        case OperationKind::RemoveDevices:
            wl_seat_send_capabilities(seat_, 0);
            break;
        case OperationKind::Fault:
            wl_resource_post_error(FindPopup(operation.popup).popup, XDG_POPUP_ERROR_INVALID_GRAB,
                                   "popup protocol fixture failure");
            break;
        }
    }

    void SendInput(const Surface &surface)
    {
        assert(pointer_ && keyboard_ && touch_);
        assert(wl_resource_get_client(pointer_) == wl_resource_get_client(surface.native));
        wl_pointer_send_enter(pointer_, wl_display_next_serial(display_), surface.native,
                              wl_fixed_from_int(12), wl_fixed_from_int(14));
        wl_pointer_send_motion(pointer_, 1, wl_fixed_from_int(15), wl_fixed_from_int(16));
        wl_pointer_send_button(pointer_, wl_display_next_serial(display_), 2, 272,
                               WL_POINTER_BUTTON_STATE_PRESSED);
        wl_pointer_send_button(pointer_, wl_display_next_serial(display_), 3, 272,
                               WL_POINTER_BUTTON_STATE_RELEASED);
        wl_pointer_send_axis(pointer_, 4, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(3));
        wl_pointer_send_frame(pointer_);
        wl_pointer_send_leave(pointer_, wl_display_next_serial(display_), surface.native);

        wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(keyboard_, wl_display_next_serial(display_), surface.native, &keys);
        wl_array_release(&keys);
        wl_keyboard_send_key(keyboard_, wl_display_next_serial(display_), 5, 30,
                             WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_keyboard_send_key(keyboard_, wl_display_next_serial(display_), 6, 30,
                             WL_KEYBOARD_KEY_STATE_RELEASED);
        wl_keyboard_send_leave(keyboard_, wl_display_next_serial(display_), surface.native);

        wl_touch_send_down(touch_, wl_display_next_serial(display_), 7, surface.native, 5,
                           wl_fixed_from_int(18), wl_fixed_from_int(19));
        wl_touch_send_motion(touch_, 8, 5, wl_fixed_from_int(20), wl_fixed_from_int(21));
        wl_touch_send_up(touch_, wl_display_next_serial(display_), 9, 5);
        wl_touch_send_frame(touch_);
        wl_touch_send_cancel(touch_);
    }

    static void Destroy(wl_client *, wl_resource *resource)
    {
        wl_resource_destroy(resource);
    }

    static void DestroySurface(wl_resource *resource)
    {
        auto *surface = static_cast<Surface *>(wl_resource_get_user_data(resource));
        auto &server = *surface->server;
        if (surface->role == Role::Popup) {
            server.state_.popups[surface->popup_index].surface_destroy_order =
                ++server.state_.destroy_order;
        }
        if (surface->xdg) {
            wl_resource_set_user_data(surface->xdg, nullptr);
        }
        if (surface->toplevel) {
            wl_resource_set_user_data(surface->toplevel, nullptr);
        }
        if (surface->popup) {
            wl_resource_set_user_data(surface->popup, nullptr);
        }
        if (surface->effect) {
            wl_resource_set_user_data(surface->effect, nullptr);
        }
        if (server.parent_ == surface) {
            server.parent_ = nullptr;
        }
        for (auto *frame : surface->frames) {
            wl_resource_destroy(frame);
        }
        const auto feedbacks = server.feedbacks_;
        for (auto *feedback : feedbacks) {
            if (feedback->surface == surface) {
                wl_resource_destroy(feedback->resource);
            }
        }
        std::erase(server.surfaces_, surface);
        --server.state_.live_surfaces;
        delete surface;
    }

    static void DestroyXdg(wl_resource *resource)
    {
        if (auto *surface = static_cast<Surface *>(wl_resource_get_user_data(resource))) {
            if (surface->role == Role::Popup) {
                surface->server->state_.popups[surface->popup_index].xdg_destroy_order =
                    ++surface->server->state_.destroy_order;
            }
            surface->xdg = nullptr;
        }
    }

    static void DestroyToplevel(wl_resource *resource)
    {
        if (auto *surface = static_cast<Surface *>(wl_resource_get_user_data(resource))) {
            surface->toplevel = nullptr;
        }
    }

    static void DestroyPopup(wl_resource *resource)
    {
        if (auto *surface = static_cast<Surface *>(wl_resource_get_user_data(resource))) {
            surface->server->state_.popups[surface->popup_index].popup_destroy_order =
                ++surface->server->state_.destroy_order;
            surface->popup = nullptr;
        }
    }

    static void Attach(wl_client *, wl_resource *resource, wl_resource *buffer, int, int)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        surface.pending_buffer = buffer;
        surface.attached = true;
        if (surface.role == Role::Popup) {
            auto &popup = surface.server->state_.popups[surface.popup_index];
            ++popup.attachments;
            popup.attachment_order = ++surface.server->state_.protocol_order;
            assert(buffer && surface.configured && popup.acks > 0);
            auto *shm = wl_shm_buffer_get(buffer);
            assert(shm);
            popup.buffer_width = wl_shm_buffer_get_width(shm);
            popup.buffer_height = wl_shm_buffer_get_height(shm);
            assert(popup.geometry_requests && popup.window_geometry.x >= 0 &&
                   popup.window_geometry.y >= 0 && popup.window_geometry.width > 0 &&
                   popup.window_geometry.height > 0);
            assert(popup.window_geometry.x + popup.window_geometry.width <= popup.buffer_width &&
                   popup.window_geometry.y + popup.window_geometry.height <= popup.buffer_height);
        }
    }

    static void Commit(wl_client *, wl_resource *resource)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        auto &server = *surface.server;
        if (surface.role == Role::Parent && !surface.configured) {
            wl_array states;
            wl_array_init(&states);
            xdg_toplevel_send_configure(surface.toplevel, 320, 200, &states);
            wl_array_release(&states);
            xdg_surface_send_configure(surface.xdg, wl_display_next_serial(server.display_));
            surface.configured = true;
        }
        if (surface.role == Role::Popup) {
            auto &popup = server.state_.popups[surface.popup_index];
            popup.commit_order = ++server.state_.protocol_order;
            if (surface.effect) {
                popup.current_effects = popup.pending_effects;
                ++popup.effect_commits;
            }
            if (surface.attached && surface.pending_buffer) {
                ++popup.buffer_commits;
            } else {
                ++popup.empty_commits;
            }
        } else if (surface.attached && surface.pending_buffer) {
            ++server.state_.parent_buffer_commits;
        }
        if (surface.pending_buffer) {
            wl_buffer_send_release(surface.pending_buffer);
        }
        surface.pending_buffer = nullptr;
        surface.attached = false;
        for (auto *feedback : server.feedbacks_) {
            if (feedback->surface == &surface) {
                feedback->committed = true;
            }
        }
        if (surface.role != Role::Popup || !server.hold_child_frames_) {
            FinishFrames(surface);
        }
    }

    static void EffectDestroyed(wl_resource *resource)
    {
        auto *surface = static_cast<Surface *>(wl_resource_get_user_data(resource));
        if (!surface) {
            return;
        }
        auto &server = *surface->server;
        auto &popup = server.state_.popups[surface->popup_index];
        popup.effect_destroy_order = ++server.state_.destroy_order;
        surface->effect = nullptr;
    }

    static void ClearEffects(wl_client *, wl_resource *resource)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        auto &server = *surface.server;
        auto &popup = server.state_.popups[surface.popup_index];
        ++popup.effect_clears;
        popup.pending_effects.clear();
        popup.effect_request_order = ++server.state_.protocol_order;
    }

    static void AddEffectRectangle(wl_client *, wl_resource *resource, wl_fixed_t x, wl_fixed_t y,
                                   wl_fixed_t width, wl_fixed_t height, wl_fixed_t corner_radius,
                                   wl_fixed_t blur_radius)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        auto &server = *surface.server;
        auto &popup = server.state_.popups[surface.popup_index];
        const contracts::SurfaceEffectRegion region{{wl_fixed_to_double(x), wl_fixed_to_double(y),
                                                     wl_fixed_to_double(width),
                                                     wl_fixed_to_double(height)},
                                                    wl_fixed_to_double(corner_radius),
                                                    wl_fixed_to_double(blur_radius)};
        contracts::ValidateSurfaceEffectRegion(region);
        ++popup.effect_rectangles;
        popup.pending_effects.push_back(region);
        popup.effect_request_order = ++server.state_.protocol_order;
    }

    static void AddEffectContour(wl_client *, wl_resource *resource, wl_fixed_t blur_radius,
                                 wl_array *payload)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        auto &server = *surface.server;
        assert(server.contour_ && wl_resource_get_version(resource) >= 2 && payload);
        const auto bytes =
            std::span(static_cast<const std::uint8_t *>(payload->data), payload->size);
        auto contour = contracts::DecodeContour(bytes);
        contracts::SurfaceEffectRegion region{contracts::ContourBounds(contour), 0,
                                              wl_fixed_to_double(blur_radius), std::move(contour)};
        contracts::ValidateSurfaceEffectRegion(region);
        auto &popup = server.state_.popups[surface.popup_index];
        ++popup.effect_contours;
        popup.pending_effects.push_back(std::move(region));
        popup.last_effect_payload.assign(bytes.begin(), bytes.end());
        popup.effect_request_order = ++server.state_.protocol_order;
    }

    static void GetEffect(wl_client *client, wl_resource *manager, std::uint32_t id,
                          wl_resource *native)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(native));
        assert(surface.role == Role::Popup && !surface.effect);
        surface.effect = wl_resource_create(client, &prism_surface_effect_v1_interface,
                                            wl_resource_get_version(manager), id);
        assert(surface.effect);
        static const struct prism_surface_effect_v1_interface implementation{
            Destroy, ClearEffects, AddEffectRectangle, AddEffectContour};
        wl_resource_set_implementation(surface.effect, &implementation, &surface, EffectDestroyed);
        ++surface.server->state_.popups[surface.popup_index].effect_creates;
    }

    static void BindEffects(wl_client *client, void *data, std::uint32_t version, std::uint32_t id)
    {
        auto &server = *static_cast<Server *>(data);
        const auto bound_version = std::min(version, server.effects_version_);
        auto *resource = wl_resource_create(client, &prism_surface_effect_manager_v1_interface,
                                            bound_version, id);
        assert(resource);
        static const struct prism_surface_effect_manager_v1_interface implementation{Destroy,
                                                                                     GetEffect};
        wl_resource_set_implementation(resource, &implementation, &server, nullptr);
        prism_surface_effect_manager_v1_send_capabilities(resource, server.backdrop_);
        if (bound_version >= 2) {
            prism_surface_effect_manager_v1_send_contour_capabilities(resource, server.contour_);
        }
        if (bound_version >= 3) {
            prism_surface_effect_manager_v1_send_popup_backdrop_capabilities(
                resource, server.popup_backdrop_);
        }
    }

    static void FinishFrames(Surface &surface)
    {
        for (auto *frame : surface.frames) {
            wl_callback_send_done(frame, 1);
            wl_resource_destroy(frame);
        }
        surface.frames.clear();
    }

    static void Frame(wl_client *client, wl_resource *resource, std::uint32_t id)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        auto *frame = wl_resource_create(client, &wl_callback_interface, 1, id);
        assert(frame);
        surface.frames.push_back(frame);
        if (surface.role == Role::Popup) {
            ++surface.server->state_.popups[surface.popup_index].frame_requests;
        }
    }

    static void DestroyFeedback(wl_resource *resource)
    {
        auto *feedback = static_cast<Feedback *>(wl_resource_get_user_data(resource));
        std::erase(feedback->surface->server->feedbacks_, feedback);
        delete feedback;
    }

    static void PresentationFeedback(wl_client *client, wl_resource *, wl_resource *native,
                                     std::uint32_t id)
    {
        auto *surface = static_cast<Surface *>(wl_resource_get_user_data(native));
        auto *resource = wl_resource_create(client, &wp_presentation_feedback_interface, 1, id);
        assert(resource);
        auto *feedback = new Feedback{surface, resource, false};
        wl_resource_set_implementation(resource, nullptr, feedback, DestroyFeedback);
        surface->server->feedbacks_.push_back(feedback);
        if (surface->role == Role::Popup) {
            ++surface->server->state_.popups[surface->popup_index].feedback_requests;
        }
    }

    static void BindPresentation(wl_client *client, void *data, std::uint32_t, std::uint32_t id)
    {
        struct Implementation {
            void (*destroy)(wl_client *, wl_resource *);
            void (*feedback)(wl_client *, wl_resource *, wl_resource *, std::uint32_t);
        };

        static const Implementation implementation{Destroy, PresentationFeedback};
        auto *resource = wl_resource_create(client, &wp_presentation_interface, 1, id);
        assert(resource);
        wl_resource_set_implementation(resource, &implementation, data, nullptr);
        wl_resource_post_event(resource, 0, 1u);
    }

    static void Damage(wl_client *, wl_resource *resource, int x, int y, int width, int height)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        if (surface.role == Role::Popup) {
            auto &popup = surface.server->state_.popups[surface.popup_index];
            ++popup.damage_requests;
            popup.damage = {double(x), double(y), double(width), double(height)};
            popup.damage_order = ++surface.server->state_.protocol_order;
        }
    }

    static void RegionState(wl_client *, wl_resource *resource, wl_resource *)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        if (surface.role == Role::Popup) {
            ++surface.server->state_.popups[surface.popup_index].input_requests;
        }
    }

    static void IntegerState(wl_client *, wl_resource *, int)
    {
    }

    static void CreateSurface(wl_client *client, wl_resource *resource, std::uint32_t id)
    {
        auto &server = *static_cast<Server *>(wl_resource_get_user_data(resource));
        auto *surface = new Surface{};
        surface->server = &server;
        surface->native = wl_resource_create(client, &wl_surface_interface, 4, id);
        assert(surface->native);
        static const struct wl_surface_interface implementation{.destroy = Destroy,
                                                                .attach = Attach,
                                                                .damage = Damage,
                                                                .frame = Frame,
                                                                .set_opaque_region = RegionState,
                                                                .set_input_region = RegionState,
                                                                .commit = Commit,
                                                                .set_buffer_transform =
                                                                    IntegerState,
                                                                .set_buffer_scale = IntegerState,
                                                                .damage_buffer = Damage};
        wl_resource_set_implementation(surface->native, &implementation, surface, DestroySurface);
        server.surfaces_.push_back(surface);
        ++server.state_.surfaces;
        ++server.state_.live_surfaces;
    }

    static void RegionRect(wl_client *, wl_resource *, int, int, int, int)
    {
    }

    static void CreateRegion(wl_client *client, wl_resource *, std::uint32_t id)
    {
        static const struct wl_region_interface implementation{Destroy, RegionRect, RegionRect};
        auto *region = wl_resource_create(client, &wl_region_interface, 1, id);
        assert(region);
        wl_resource_set_implementation(region, &implementation, nullptr, nullptr);
    }

    static void BindCompositor(wl_client *client, void *data, std::uint32_t version,
                               std::uint32_t id)
    {
        auto &server = *static_cast<Server *>(data);
        ++server.state_.clients;
        static const struct wl_compositor_interface implementation{CreateSurface, CreateRegion};
        auto *resource =
            wl_resource_create(client, &wl_compositor_interface, std::min(version, 4u), id);
        assert(resource);
        wl_resource_set_implementation(resource, &implementation, data, nullptr);
    }

    static void StringState(wl_client *, wl_resource *, const char *)
    {
    }

    static void EmptyState(wl_client *, wl_resource *)
    {
    }

    static void GetToplevel(wl_client *client, wl_resource *resource, std::uint32_t id)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        static const struct xdg_toplevel_interface implementation{.destroy = Destroy,
                                                                  .set_title = StringState,
                                                                  .set_app_id = StringState,
                                                                  .set_maximized = EmptyState,
                                                                  .unset_maximized = EmptyState};
        surface.toplevel = wl_resource_create(client, &xdg_toplevel_interface, 1, id);
        assert(surface.toplevel && !surface.server->parent_);
        wl_resource_set_implementation(surface.toplevel, &implementation, &surface,
                                       DestroyToplevel);
        surface.role = Role::Parent;
        surface.server->parent_ = &surface;
        ++surface.server->state_.toplevels;
    }

    static void Grab(wl_client *, wl_resource *resource, wl_resource *, std::uint32_t)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        ++surface.server->state_.grabs;
    }

    static void Reposition(wl_client *, wl_resource *resource, wl_resource *, std::uint32_t)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        ++surface.server->state_.repositions;
    }

    static void GetPopup(wl_client *client, wl_resource *resource, std::uint32_t id,
                         wl_resource *parent, wl_resource *positioner_resource)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        auto &server = *surface.server;
        const auto &positioner =
            *static_cast<Positioner *>(wl_resource_get_user_data(positioner_resource));
        auto *owner = parent ? static_cast<Surface *>(wl_resource_get_user_data(parent)) : nullptr;
        PopupState popup;
        popup.positioner = positioner.state;
        popup.correct_parent = owner == server.parent_ && owner && owner->role == Role::Parent &&
                               wl_resource_get_client(parent) == client;
        surface.popup_index = server.state_.popups.size();
        server.state_.popups.push_back(popup);
        surface.role = Role::Popup;
        surface.popup =
            wl_resource_create(client, &xdg_popup_interface, wl_resource_get_version(resource), id);
        assert(surface.popup);
        static const struct xdg_popup_interface implementation{Destroy, Grab, Reposition};
        wl_resource_set_implementation(surface.popup, &implementation, &surface, DestroyPopup);
    }

    static void SetGeometry(wl_client *, wl_resource *resource, int x, int y, int width, int height)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        if (surface.role == Role::Popup) {
            auto &popup = surface.server->state_.popups[surface.popup_index];
            ++popup.geometry_requests;
            popup.window_geometry = {double(x), double(y), double(width), double(height)};
            popup.geometry_order = ++surface.server->state_.protocol_order;
        }
    }

    static void AckConfigure(wl_client *, wl_resource *resource, std::uint32_t serial)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        if (surface.role == Role::Popup) {
            auto &popup = surface.server->state_.popups[surface.popup_index];
            ++popup.acks;
            popup.last_ack = serial;
        }
    }

    static void GetXdg(wl_client *client, wl_resource *shell, std::uint32_t id, wl_resource *native)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(native));
        surface.xdg =
            wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(shell), id);
        assert(surface.xdg);
        static const struct xdg_surface_interface implementation{Destroy, GetToplevel, GetPopup,
                                                                 SetGeometry, AckConfigure};
        wl_resource_set_implementation(surface.xdg, &implementation, &surface, DestroyXdg);
    }

    static Positioner &PositionerData(wl_resource *resource)
    {
        return *static_cast<Positioner *>(wl_resource_get_user_data(resource));
    }

    static void PositionerSize(wl_client *, wl_resource *resource, int width, int height)
    {
        auto &state = PositionerData(resource).state;
        state.width = width;
        state.height = height;
        ++state.size_calls;
    }

    static void PositionerRect(wl_client *, wl_resource *resource, int x, int y, int width,
                               int height)
    {
        auto &state = PositionerData(resource).state;
        state.anchor = {x, y, width, height};
        ++state.anchor_rect_calls;
    }

    static void PositionerAnchor(wl_client *, wl_resource *resource, std::uint32_t anchor)
    {
        auto &state = PositionerData(resource).state;
        state.anchor_edge = anchor;
        ++state.anchor_calls;
    }

    static void PositionerGravity(wl_client *, wl_resource *resource, std::uint32_t gravity)
    {
        auto &state = PositionerData(resource).state;
        state.gravity = gravity;
        ++state.gravity_calls;
    }

    static void PositionerConstraints(wl_client *, wl_resource *resource, std::uint32_t constraints)
    {
        auto &state = PositionerData(resource).state;
        state.constraints = constraints;
        ++state.constraint_calls;
    }

    static void PositionerOffset(wl_client *, wl_resource *resource, int x, int y)
    {
        auto &state = PositionerData(resource).state;
        state.offset_x = x;
        state.offset_y = y;
        ++state.offset_calls;
    }

    static void PositionerReactive(wl_client *, wl_resource *resource)
    {
        ++PositionerData(resource).state.extension_calls;
    }

    static void PositionerParentSize(wl_client *, wl_resource *resource, int, int)
    {
        ++PositionerData(resource).state.extension_calls;
    }

    static void PositionerParentConfigure(wl_client *, wl_resource *resource, std::uint32_t)
    {
        ++PositionerData(resource).state.extension_calls;
    }

    static void DestroyPositioner(wl_resource *resource)
    {
        auto *positioner = static_cast<Positioner *>(wl_resource_get_user_data(resource));
        ++positioner->server->state_.positioner_destroys;
        delete positioner;
    }

    static void CreatePositioner(wl_client *client, wl_resource *shell, std::uint32_t id)
    {
        auto *server = static_cast<Server *>(wl_resource_get_user_data(shell));
        auto *positioner = new Positioner{server, {}};
        auto *resource = wl_resource_create(client, &xdg_positioner_interface,
                                            wl_resource_get_version(shell), id);
        assert(resource);
        static const struct xdg_positioner_interface implementation{Destroy,
                                                                    PositionerSize,
                                                                    PositionerRect,
                                                                    PositionerAnchor,
                                                                    PositionerGravity,
                                                                    PositionerConstraints,
                                                                    PositionerOffset,
                                                                    PositionerReactive,
                                                                    PositionerParentSize,
                                                                    PositionerParentConfigure};
        wl_resource_set_implementation(resource, &implementation, positioner, DestroyPositioner);
        ++server->state_.positioners;
    }

    static void Pong(wl_client *, wl_resource *, std::uint32_t)
    {
    }

    static void BindShell(wl_client *client, void *data, std::uint32_t version, std::uint32_t id)
    {
        auto *resource =
            wl_resource_create(client, &xdg_wm_base_interface, std::min(version, 3u), id);
        assert(resource);
        static const struct xdg_wm_base_interface implementation{Destroy, CreatePositioner, GetXdg,
                                                                 Pong};
        wl_resource_set_implementation(resource, &implementation, data, nullptr);
    }

    static void Cursor(wl_client *, wl_resource *, std::uint32_t, wl_resource *, int, int)
    {
    }

    static void GetPointer(wl_client *client, wl_resource *seat, std::uint32_t id)
    {
        auto &server = *static_cast<Server *>(wl_resource_get_user_data(seat));
        server.pointer_ = wl_resource_create(client, &wl_pointer_interface, 5, id);
        assert(server.pointer_);
        static const struct wl_pointer_interface implementation{Cursor, Destroy};
        wl_resource_set_implementation(server.pointer_, &implementation, &server, nullptr);
        ++server.state_.pointers;
    }

    static void GetKeyboard(wl_client *client, wl_resource *seat, std::uint32_t id)
    {
        auto &server = *static_cast<Server *>(wl_resource_get_user_data(seat));
        server.keyboard_ = wl_resource_create(client, &wl_keyboard_interface, 5, id);
        assert(server.keyboard_);
        static const struct wl_keyboard_interface implementation{Destroy};
        wl_resource_set_implementation(server.keyboard_, &implementation, &server, nullptr);
        ++server.state_.keyboards;
    }

    static void GetTouch(wl_client *client, wl_resource *seat, std::uint32_t id)
    {
        auto &server = *static_cast<Server *>(wl_resource_get_user_data(seat));
        server.touch_ = wl_resource_create(client, &wl_touch_interface, 5, id);
        assert(server.touch_);
        static const struct wl_touch_interface implementation{Destroy};
        wl_resource_set_implementation(server.touch_, &implementation, &server, nullptr);
        ++server.state_.touches;
    }

    static void BindSeat(wl_client *client, void *data, std::uint32_t version, std::uint32_t id)
    {
        auto &server = *static_cast<Server *>(data);
        auto *resource = wl_resource_create(client, &wl_seat_interface, std::min(version, 5u), id);
        assert(resource);
        static const struct wl_seat_interface implementation{GetPointer, GetKeyboard, GetTouch,
                                                             Destroy};
        wl_resource_set_implementation(resource, &implementation, data, nullptr);
        server.seat_ = resource;
        wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER |
                                                WL_SEAT_CAPABILITY_KEYBOARD |
                                                WL_SEAT_CAPABILITY_TOUCH);
        ++server.state_.seats;
    }

    wl_display *display_{};
    wl_resource *seat_{}, *pointer_{}, *keyboard_{}, *touch_{};
    Surface *parent_{};
    std::vector<Surface *> surfaces_;
    std::vector<Feedback *> feedbacks_;
    bool hold_child_frames_{};
    unsigned effects_version_{};
    bool popup_backdrop_{}, contour_{}, backdrop_{};
    ServerState state_;
    std::atomic<bool> stop_{};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Operation>> operations_;
};

struct WindowEvents {
    std::vector<contracts::WindowEvent> events;

    void Handle(const contracts::WindowEvent &event)
    {
        events.push_back(event);
    }

    template <typename Event> unsigned Count() const
    {
        return std::count_if(events.begin(), events.end(), [](const contracts::WindowEvent &event) {
            return std::holds_alternative<Event>(event);
        });
    }
};

struct PopupEvents {
    WaylandWindow *parent{};
    std::vector<platform::WaylandPopupConfigure> configurations;
    std::vector<WaylandPopupCloseReason> closures;
    bool parent_alive_at_close{};

    void Handle(const platform::WaylandPopupEvent &event)
    {
        if (const auto *configure = std::get_if<platform::WaylandPopupConfigure>(&event)) {
            configurations.push_back(*configure);
        } else if (const auto *closed = std::get_if<platform::WaylandPopupClosed>(&event)) {
            const auto reason = closed->reason;
            closures.push_back(reason);
            if (reason == WaylandPopupCloseReason::ParentClosed) {
                parent_alive_at_close = parent && parent->Display() && parent->Surface();
            }
        }
    }
};

void Paint(void *data, int width, int height, int stride)
{
    auto *pixels = static_cast<std::uint32_t *>(data);
    for (int y = 0; y < height; ++y) {
        std::fill_n(pixels + y * stride / 4, width, 0xFF557788);
    }
}

template <typename Predicate> void Until(WaylandWindow &window, Predicate ready)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!ready()) {
        assert(std::chrono::steady_clock::now() < deadline);
        assert(window.Pump(5));
    }
}

bool Until(WaylandWindow &window, const bool &done)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!done) {
        assert(std::chrono::steady_clock::now() < deadline);
        if (!window.Pump(5)) {
            return false;
        }
    }
    return true;
}

void SyncDone(void *data, wl_callback *, std::uint32_t)
{
    *static_cast<bool *>(data) = true;
}

void Roundtrip(WaylandWindow &window)
{
    auto *display = window.Display();
    assert(display);
    auto *callback = wl_display_sync(display);
    bool done = false;
    static const wl_callback_listener listener{SyncDone};
    assert(callback && wl_callback_add_listener(callback, &listener, &done) == 0);

    const bool completed = Until(window, done);
    if (window.Display() == display) {
        wl_callback_destroy(callback);
    }
    assert(completed && done && window.Display() == display);
}

class Parent {
public:
    Parent(Server &server, const std::string &socket) : server(server)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (server.Inspect().live_surfaces != 0) {
            assert(std::chrono::steady_clock::now() < deadline);
        }
        window.SetPaintHandler(Paint);
        window.SetEventHandler(std::bind_front(&WindowEvents::Handle, &events));
        assert(window.Open(socket, "prism.popup-fixture", "Popup fixture", 640, 400));
        Until(window, [this] { return window.IsConfigured() && window.IsMapped(); });
        Roundtrip(window);
        assert(window.Metrics().logical_size == contracts::LogicalSize(320, 200));
        initial = server.Inspect();
        events.events.clear();
    }

    ~Parent()
    {
        window.Close();
    }

    Server &server;
    ServerState initial;
    WindowEvents events;
    WaylandWindow window;
};

class Buffers {
    struct Buffer {
        wl_buffer *handle{};
        void *pixels{MAP_FAILED};
        std::size_t bytes{};

        ~Buffer()
        {
            wl_buffer_destroy(handle);
            munmap(pixels, bytes);
        }
    };

public:
    explicit Buffers(WaylandWindow &parent)
    {
        registry_ = wl_display_get_registry(parent.Display());
        static const wl_registry_listener listener{Global, GlobalRemoved};
        assert(registry_ && wl_registry_add_listener(registry_, &listener, this) == 0);
        Roundtrip(parent);
        assert(shm_);
    }

    ~Buffers()
    {
        Close();
    }

    wl_buffer *Create(int width, int height)
    {
        auto buffer = std::make_unique<Buffer>();
        buffer->bytes = static_cast<std::size_t>(width) * height * 4;
        const int fd = memfd_create("prism-popup-test", MFD_CLOEXEC);
        assert(fd >= 0 && ftruncate(fd, static_cast<off_t>(buffer->bytes)) == 0);
        buffer->pixels = mmap(nullptr, buffer->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(buffer->pixels != MAP_FAILED);
        std::fill_n(static_cast<std::uint32_t *>(buffer->pixels), width * height, 0xFF7799AA);
        auto *pool = wl_shm_create_pool(shm_, fd, static_cast<int>(buffer->bytes));
        assert(pool);
        buffer->handle =
            wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        assert(buffer->handle);
        static const wl_buffer_listener listener{Released};
        assert(wl_buffer_add_listener(buffer->handle, &listener, nullptr) == 0);
        auto *handle = buffer->handle;
        buffers_.push_back(std::move(buffer));
        return handle;
    }

    void Close()
    {
        buffers_.clear();
        if (shm_) {
            wl_shm_destroy(shm_);
            shm_ = nullptr;
        }
        if (registry_) {
            wl_registry_destroy(registry_);
            registry_ = nullptr;
        }
    }

private:
    static void Released(void *, wl_buffer *)
    {
    }

    static void Global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                       std::uint32_t)
    {
        if (std::strcmp(interface, wl_shm_interface.name) == 0) {
            auto &self = *static_cast<Buffers *>(data);
            self.shm_ =
                static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        }
    }

    static void GlobalRemoved(void *, wl_registry *, std::uint32_t)
    {
    }

    wl_registry *registry_{};
    wl_shm *shm_{};
    std::vector<std::unique_ptr<Buffer>> buffers_;
};

contracts::PopupPositionerRequest Request()
{
    return {{-10.25, 20.5, 61.25, 22.25}, {160.25, 80.25}, 7.75};
}

void CheckWire(const ServerState &state, const ServerState &parent, std::size_t index,
               const contracts::PopupPositionerRequest &request)
{
    assert(state.clients == parent.clients && state.seats == parent.seats &&
           state.pointers == parent.pointers && state.keyboards == parent.keyboards &&
           state.touches == parent.touches && state.toplevels == parent.toplevels);
    assert(state.surfaces - parent.surfaces == state.popups.size() - parent.popups.size());
    assert(state.grabs == 0 && state.repositions == 0);
    const auto &popup = state.popups.at(index);
    assert(popup.correct_parent);
    const auto &wire = popup.positioner;
    assert(wire.anchor == contracts::PopupPositionerRect(0, 20, 51, 23));
    assert(wire.width == 161 && wire.height == 81);
    assert(wire.offset_x == 0 &&
           wire.offset_y ==
               (request.vertical_preference == contracts::PopupVerticalPreference::Below ? 8 : -8));
    assert(wire.size_calls == 1 && wire.anchor_rect_calls == 1 && wire.anchor_calls == 1 &&
           wire.gravity_calls == 1 && wire.constraint_calls == 1 && wire.offset_calls == 1);
    assert(wire.extension_calls == 0);
    const auto required = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                          XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                          XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                          XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X |
                          XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y;
    assert((wire.constraints & required) == required);
    const bool below = request.vertical_preference == contracts::PopupVerticalPreference::Below;
    if (request.horizontal_alignment == contracts::PopupHorizontalAlignment::Center) {
        assert(wire.anchor_edge ==
               (below ? XDG_POSITIONER_ANCHOR_BOTTOM : XDG_POSITIONER_ANCHOR_TOP));
        assert(wire.gravity ==
               (below ? XDG_POSITIONER_GRAVITY_BOTTOM : XDG_POSITIONER_GRAVITY_TOP));
    } else if (request.horizontal_alignment == contracts::PopupHorizontalAlignment::Start) {
        assert(wire.anchor_edge ==
               (below ? XDG_POSITIONER_ANCHOR_BOTTOM_LEFT : XDG_POSITIONER_ANCHOR_TOP_LEFT));
        assert(wire.gravity ==
               (below ? XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT : XDG_POSITIONER_GRAVITY_TOP_RIGHT));
    } else {
        assert(wire.anchor_edge ==
               (below ? XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT : XDG_POSITIONER_ANCHOR_TOP_RIGHT));
        assert(wire.gravity ==
               (below ? XDG_POSITIONER_GRAVITY_BOTTOM_LEFT : XDG_POSITIONER_GRAVITY_TOP_LEFT));
    }
}

void CheckDestroyed(const PopupState &state)
{
    assert(state.popup_destroy_order > 0 && state.popup_destroy_order < state.xdg_destroy_order &&
           state.xdg_destroy_order < state.surface_destroy_order);
}

void PositionerAndBufferGuards(Server &server, const std::string &socket)
{
    WaylandWindow unopened;
    WaylandPopup unavailable;
    assert(!unavailable.Open(unopened, Request()) && unavailable.IsClosed() &&
           !unavailable.Surface());

    Parent parent(server, socket);
    Buffers buffers(parent.window);
    PopupEvents events{&parent.window};
    WaylandPopup popup;
    popup.SetEventHandler(std::bind_front(&PopupEvents::Handle, &events));
    auto request = Request();
    const auto before = server.Inspect();
    request.anchor = {400, 10, 30, 20}; // Actual configure is 320 wide, preferred width was 640.
    assert(!popup.Open(parent.window, request));
    request = Request();
    request.desired.width = std::numeric_limits<double>::quiet_NaN();
    assert(!popup.Open(parent.window, request));
    request = Request();
    request.gap = -1;
    assert(!popup.Open(parent.window, request));
    Roundtrip(parent.window);
    assert(server.Inspect().surfaces == before.surfaces && events.closures.empty());

    for (auto vertical :
         {contracts::PopupVerticalPreference::Below, contracts::PopupVerticalPreference::Above}) {
        for (auto horizontal : {contracts::PopupHorizontalAlignment::Start,
                                contracts::PopupHorizontalAlignment::Center,
                                contracts::PopupHorizontalAlignment::End}) {
            request = Request();
            request.horizontal_alignment = horizontal;
            request.vertical_preference = vertical;
            const auto index = server.Inspect().popups.size();
            assert(popup.Open(parent.window, request));
            assert(!popup.Open(parent.window, request));
            assert(!popup.IsClosed() && !popup.IsConfigured() && !popup.Surface());
            auto *buffer = buffers.Create(137, 69);
            assert(!popup.AttachBuffer(buffer, 137, 69));
            Roundtrip(parent.window);
            auto state = server.Inspect();
            CheckWire(state, parent.initial, index, request);
            assert(state.popups[index].empty_commits == 1 &&
                   state.popups[index].buffer_commits == 0 &&
                   state.popups[index].attachments == 0 && state.popups[index].acks == 0);

            // The compositor may flip and resize. Its configure overrides requested dimensions.
            const contracts::LogicalRect bounds{73, -19, 137, 69};
            const auto configurations = events.configurations.size();
            server.Configure(index, bounds);
            Until(parent.window, [&popup] { return popup.IsConfigured(); });
            Roundtrip(parent.window);
            assert(popup.Surface() && popup.Configure() && popup.Configure()->bounds == bounds);
            assert(popup.Configure()->serial && popup.Configure()->generation);
            assert(events.configurations.size() == configurations + 1);
            state = server.Inspect();
            assert(state.popups[index].empty_commits == 1 && state.popups[index].acks == 1 &&
                   state.popups[index].attachments == 0 && state.popups[index].buffer_commits == 0);
            assert(!popup.AttachBuffer(nullptr, 137, 69));
            assert(!popup.AttachBuffer(buffer, 161, 81));
            assert(!popup.AttachBuffer(buffer, 0, 69));
            assert(popup.AttachBuffer(buffer, 137, 69));
            Roundtrip(parent.window);
            state = server.Inspect();
            assert(state.popups[index].acks == 1 &&
                   state.popups[index].last_ack == popup.Configure()->serial);
            assert(
                state.popups[index].attachments == 1 && state.popups[index].buffer_commits == 1 &&
                state.popups[index].buffer_width == 137 && state.popups[index].buffer_height == 69);

            const auto previous = *popup.Configure();
            server.Configure(index, {41, 33, 120, 60});
            Until(parent.window, [&popup, previous] {
                return popup.Configure()->generation > previous.generation;
            });
            assert(popup.Configure()->serial != previous.serial);
            assert(!popup.AttachBuffer(buffer, 137, 69));
            auto *resized = buffers.Create(120, 60);
            assert(popup.AttachBuffer(resized, 120, 60));
            Roundtrip(parent.window);
            assert(server.Inspect().popups[index].buffer_commits == 2);

            const auto closed = events.closures.size();
            const auto last = *popup.Configure();
            popup.Close();
            popup.Close();
            assert(popup.IsClosed() && !popup.IsConfigured() && !popup.Surface());
            assert(popup.Configure()->bounds == last.bounds &&
                   popup.Configure()->generation == last.generation);
            assert(!popup.AttachBuffer(resized, 120, 60));
            assert(events.closures.size() == closed + 1 &&
                   events.closures.back() == WaylandPopupCloseReason::Requested);
            Roundtrip(parent.window);
            state = server.Inspect();
            CheckDestroyed(state.popups[index]);
            assert(state.popups[index].empty_commits == 1 &&
                   state.popups[index].buffer_commits == 2);
        }
    }
    const auto state = server.Inspect();
    assert(state.positioners == state.positioner_destroys && state.live_surfaces == 1);
}

void RejectBufferLayout(Server &server, WaylandWindow &parent, WaylandPopup &popup,
                        const WaylandPopupBufferLayout &invalid,
                        const WaylandPopupBufferLayout &retained)
{
    const auto before = server.Inspect();
    assert(!popup.SetBufferLayout(invalid));
    assert(popup.BufferLayout() && *popup.BufferLayout() == retained);
    Roundtrip(parent);
    assert(server.Inspect() == before);
}

void BufferLayouts(Server &server, const std::string &socket)
{
    Parent parent(server, socket);
    Buffers buffers(parent.window);
    WaylandPopup popup;
    const auto index = server.Inspect().popups.size();
    assert(!popup.BufferLayout());
    assert(!popup.SetBufferLayout({{160, 80}, {0, 0, 160, 80}, 1}));
    assert(!popup.ResetBufferLayout(1));
    assert(popup.Open(parent.window, Request()));
    Roundtrip(parent.window);
    const auto unconfigured = server.Inspect();
    assert(!popup.BufferLayout());
    assert(!popup.SetBufferLayout({{160, 80}, {0, 0, 160, 80}, 1}));
    assert(!popup.ResetBufferLayout(1));
    Roundtrip(parent.window);
    assert(server.Inspect() == unconfigured);

    server.Configure(index, {20, 44, 160, 80});
    Until(parent.window, [&popup] { return popup.IsConfigured(); });
    Roundtrip(parent.window);
    const auto generation = popup.Configure()->generation;
    const WaylandPopupBufferLayout default_layout{{160, 80}, {0, 0, 160, 80}, generation};
    assert(popup.BufferLayout() && *popup.BufferLayout() == default_layout);
    auto state = server.Inspect().popups[index];
    assert(state.acks == 1 && state.geometry_requests == 0 && state.empty_commits == 1 &&
           state.attachments == 0 && state.buffer_commits == 0);

    auto *legacy_buffer = buffers.Create(160, 80);
    const auto before_mismatch = server.Inspect();
    assert(!popup.AttachBuffer(legacy_buffer, 161, 80));
    Roundtrip(parent.window);
    assert(server.Inspect() == before_mismatch);
    assert(popup.AttachBuffer(legacy_buffer, 160, 80));
    Roundtrip(parent.window);
    state = server.Inspect().popups[index];
    assert(state.geometry_requests == 1 && state.window_geometry == default_layout.window_geometry);
    assert(state.geometry_order < state.attachment_order &&
           state.attachment_order < state.damage_order && state.damage_order < state.commit_order);
    assert(state.buffer_width == 160 && state.buffer_height == 80 && state.buffer_commits == 1);
    assert(state.damage_requests == 1 && state.damage == contracts::LogicalRect(0, 0, 160, 80));

    const WaylandPopupBufferLayout padded{{188, 108}, {12, 10, 160, 80}, generation};
    const auto before_set = state;
    assert(popup.SetBufferLayout(padded));
    assert(popup.BufferLayout() && *popup.BufferLayout() == padded);
    Roundtrip(parent.window);
    state = server.Inspect().popups[index];
    assert(state.geometry_requests == before_set.geometry_requests + 1 &&
           state.window_geometry == padded.window_geometry);
    assert(state.attachments == before_set.attachments &&
           state.damage_requests == before_set.damage_requests &&
           state.buffer_commits == before_set.buffer_commits &&
           state.empty_commits == before_set.empty_commits);
    const auto same_layout = server.Inspect();
    assert(popup.SetBufferLayout(padded));
    Roundtrip(parent.window);
    assert(server.Inspect() == same_layout);

    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto infinity = std::numeric_limits<double>::infinity();
    const std::array invalid{
        WaylandPopupBufferLayout{{188, 108}, {12, 10, 160, 80}, 0},
        WaylandPopupBufferLayout{{188, 108}, {12, 10, 160, 80}, generation + 1},
        WaylandPopupBufferLayout{{0, 108}, {12, 10, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 0}, {12, 10, 160, 80}, generation},
        WaylandPopupBufferLayout{{4097, 108}, {12, 10, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 4097}, {12, 10, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {-1, 10, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12, -1, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12.5, 10, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12, 10.5, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12, 10, 159, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12, 10, 160, 79}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12, 10, 160.5, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12, 10, 160, 80.5}, generation},
        WaylandPopupBufferLayout{{188, 108}, {nan, 10, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12, infinity, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12, 10, nan, 80}, generation},
        WaylandPopupBufferLayout{{188, 108}, {12, 10, 160, infinity}, generation},
        WaylandPopupBufferLayout{{171, 108}, {12, 10, 160, 80}, generation},
        WaylandPopupBufferLayout{{188, 89}, {12, 10, 160, 80}, generation}};
    for (const auto &layout : invalid) {
        RejectBufferLayout(server, parent.window, popup, layout, padded);
    }
    const auto before_failed_attach = server.Inspect();
    assert(!popup.AttachBuffer(nullptr, 188, 108));
    assert(!popup.AttachBuffer(legacy_buffer, 160, 80));
    assert(!popup.AttachBuffer(legacy_buffer, -1, 108));
    assert(!popup.AttachBuffer(legacy_buffer, 188, 0));
    assert(!popup.ResetBufferLayout(generation + 1));
    assert(popup.BufferLayout() && *popup.BufferLayout() == padded);
    Roundtrip(parent.window);
    assert(server.Inspect() == before_failed_attach);

    auto *padded_buffer = buffers.Create(188, 108);
    assert(popup.AttachBuffer(padded_buffer, 188, 108));
    Roundtrip(parent.window);
    state = server.Inspect().popups[index];
    assert(state.geometry_requests == 2 && state.window_geometry == padded.window_geometry);
    assert(state.buffer_width == 188 && state.buffer_height == 108 && state.buffer_commits == 2);
    assert(state.damage_requests == 2 && state.damage == contracts::LogicalRect(0, 0, 188, 108));
    const auto before_reset = state;
    assert(popup.ResetBufferLayout(generation));
    assert(popup.BufferLayout() && *popup.BufferLayout() == default_layout);
    Roundtrip(parent.window);
    state = server.Inspect().popups[index];
    assert(state.geometry_requests == before_reset.geometry_requests + 1 &&
           state.window_geometry == default_layout.window_geometry);
    assert(state.attachments == before_reset.attachments &&
           state.damage_requests == before_reset.damage_requests &&
           state.buffer_commits == before_reset.buffer_commits &&
           state.empty_commits == before_reset.empty_commits);
    const auto after_reset = server.Inspect();
    assert(popup.ResetBufferLayout(generation));
    assert(!popup.AttachBuffer(padded_buffer, 188, 108));
    Roundtrip(parent.window);
    assert(server.Inspect() == after_reset);
    assert(popup.AttachBuffer(legacy_buffer, 160, 80));
    Roundtrip(parent.window);

    assert(popup.SetBufferLayout(padded));
    Roundtrip(parent.window);
    const auto before_configure = server.Inspect().popups[index];
    server.Configure(index, {30, 51, 120, 60});
    Until(parent.window,
          [&popup, generation] { return popup.Configure()->generation > generation; });
    Roundtrip(parent.window);
    const auto new_generation = popup.Configure()->generation;
    const WaylandPopupBufferLayout new_default{{120, 60}, {0, 0, 120, 60}, new_generation};
    assert(popup.Configure()->bounds == contracts::LogicalRect(30, 51, 120, 60));
    assert(popup.BufferLayout() && *popup.BufferLayout() == new_default);
    state = server.Inspect().popups[index];
    assert(state.acks == before_configure.acks + 1 &&
           state.geometry_requests == before_configure.geometry_requests &&
           state.window_geometry == before_configure.window_geometry &&
           state.attachments == before_configure.attachments &&
           state.damage_requests == before_configure.damage_requests &&
           state.buffer_commits == before_configure.buffer_commits &&
           state.empty_commits == before_configure.empty_commits);
    RejectBufferLayout(server, parent.window, popup, padded, new_default);
    const auto before_stale_attach = server.Inspect();
    assert(!popup.AttachBuffer(padded_buffer, 188, 108));
    assert(!popup.AttachBuffer(legacy_buffer, 160, 80));
    assert(!popup.ResetBufferLayout(generation));
    Roundtrip(parent.window);
    assert(server.Inspect() == before_stale_attach);
    auto *new_buffer = buffers.Create(120, 60);
    assert(popup.AttachBuffer(new_buffer, 120, 60));
    Roundtrip(parent.window);
    state = server.Inspect().popups[index];
    assert(state.geometry_requests == before_configure.geometry_requests + 1 &&
           state.window_geometry == new_default.window_geometry);
    assert(state.geometry_order < state.attachment_order &&
           state.attachment_order < state.damage_order && state.damage_order < state.commit_order);
    assert(state.buffer_width == 120 && state.buffer_height == 60);
    assert(state.damage_requests == before_configure.damage_requests + 1 &&
           state.damage == contracts::LogicalRect(0, 0, 120, 60));

    popup.Close();
    assert(!popup.BufferLayout());
    Roundtrip(parent.window);
    CheckDestroyed(server.Inspect().popups[index]);
    const auto closed = server.Inspect();
    assert(!popup.SetBufferLayout(new_default));
    assert(!popup.ResetBufferLayout(new_generation));
    assert(!popup.AttachBuffer(new_buffer, 120, 60));
    popup.Close();
    Roundtrip(parent.window);
    assert(!popup.BufferLayout() && server.Inspect() == closed);
}

void ChildInputAndDismissal(Server &server, const std::string &socket)
{
    Parent parent(server, socket);
    Buffers buffers(parent.window);
    PopupEvents events{&parent.window};
    WaylandPopup popup;
    popup.SetEventHandler(std::bind_front(&PopupEvents::Handle, &events));
    auto index = server.Inspect().popups.size();
    assert(popup.Open(parent.window, Request()));
    Roundtrip(parent.window);
    server.Configure(index, {20, 44, 160, 80});
    Until(parent.window, [&popup] { return popup.IsConfigured(); });
    Roundtrip(parent.window);
    auto *buffer = buffers.Create(160, 80);
    assert(popup.AttachBuffer(buffer, 160, 80));
    Roundtrip(parent.window);
    parent.events.events.clear();

    // Child coordinates and focus must never enter the root event handler.
    server.Input(true, index);
    Roundtrip(parent.window);
    assert(parent.events.events.empty());
    assert(popup.IsConfigured() && events.closures.empty());
    server.Input(false);
    Roundtrip(parent.window);
    assert(parent.events.Count<contracts::PointerEnterEvent>() == 1);
    assert(parent.events.Count<contracts::PointerMotionEvent>() == 1);
    assert(parent.events.Count<contracts::PointerButtonEvent>() == 2);
    assert(parent.events.Count<contracts::PointerScrollEvent>() == 1);
    assert(parent.events.Count<contracts::PointerLeaveEvent>() == 1);
    assert(parent.events.Count<contracts::FocusEvent>() == 2);
    assert(parent.events.Count<contracts::KeyEvent>() == 2);
    assert(parent.events.Count<contracts::TouchDownEvent>() == 1);
    assert(parent.events.Count<contracts::TouchMotionEvent>() == 1);
    assert(parent.events.Count<contracts::TouchUpEvent>() == 1);
    assert(parent.events.Count<contracts::TouchFrameEvent>() == 1);
    for (const auto &event : parent.events.events) {
        if (const auto *button = std::get_if<contracts::PointerButtonEvent>(&event)) {
            assert(button->position == contracts::LogicalPoint(15, 16) && button->protocol_serial);
        }
    }
    const auto root_events = parent.events.events.size();
    server.Input(true, index);
    Roundtrip(parent.window);
    assert(parent.events.events.size() == root_events);
    CheckWire(server.Inspect(), parent.initial, index, Request());

    const auto last = *popup.Configure();
    const auto configurations = events.configurations.size();
    server.Done(index, true); // Already queued old configure must not resurrect a dismissed child.
    Until(parent.window, [&popup] { return popup.IsClosed(); });
    Roundtrip(parent.window);
    assert(!popup.Surface() && !popup.IsConfigured());
    assert(!popup.AttachBuffer(buffer, 160, 80));
    assert(events.closures.size() == 1 &&
           events.closures[0] == WaylandPopupCloseReason::CompositorDismissed);
    assert(events.configurations.size() == configurations &&
           popup.Configure()->bounds == last.bounds);
    CheckDestroyed(server.Inspect().popups[index]);
    assert(parent.window.IsMapped() && !parent.window.IsCloseRequested());

    index = server.Inspect().popups.size();
    assert(popup.Open(parent.window, Request()));
    assert(!popup.IsConfigured() && !popup.Surface());
    Roundtrip(parent.window);
    server.Configure(index, {30, 51, 140, 70});
    Until(parent.window, [&popup] { return popup.IsConfigured(); });
    assert(popup.Configure()->bounds == contracts::LogicalRect(30, 51, 140, 70));
    assert(events.configurations.size() == configurations + 1);
    popup.Close();
    Roundtrip(parent.window);
    assert(events.closures.size() == 2);
    CheckDestroyed(server.Inspect().popups[index]);
}

platform::SubmitResult AwaitPixels(const platform::SubmitRequest &)
{
    return platform::SubmitResult::AwaitFrame;
}

void ParentCommittedGeometry(Server &server, const std::string &socket)
{
    Parent parent(server, socket);
    PopupEvents events{&parent.window};
    WaylandPopup popup;
    popup.SetEventHandler(std::bind_front(&PopupEvents::Handle, &events));
    const auto index = server.Inspect().popups.size();
    assert(popup.Open(parent.window, Request()));
    Roundtrip(parent.window);
    server.Configure(index, {20, 44, 160, 80});
    Until(parent.window, [&popup] { return popup.IsConfigured(); });
    Roundtrip(parent.window);

    const auto configure_count = parent.window.ConfigureCount();
    const auto pixels = server.Inspect().parent_buffer_commits;
    const auto submissions = parent.window.GetSubmitStats();
    parent.window.SetSubmitHandlers(AwaitPixels, {});
    server.ResizeParent(400, 240);
    Until(parent.window,
          [&parent, configure_count] { return parent.window.ConfigureCount() > configure_count; });
    Roundtrip(parent.window);
    assert(parent.window.IsConfigured() && parent.window.IsMapped());
    assert(parent.window.Metrics().logical_size == contracts::LogicalSize(400, 240));
    assert(parent.window.GetSubmitStats().state_commits > submissions.state_commits);
    assert(server.Inspect().parent_buffer_commits == pixels);
    assert(popup.IsClosed() && !popup.Surface() && events.closures.size() == 1);
    const auto resources = server.Inspect().surfaces;
    assert(!popup.Open(parent.window, Request()));
    Roundtrip(parent.window);
    assert(server.Inspect().surfaces == resources);

    // State acknowledged the resize, but only a real pixel commit authorizes a new popup.
    parent.window.SetSubmitHandlers({}, {});
    parent.window.RequestRedraw();
    Until(parent.window,
          [&server, pixels] { return server.Inspect().parent_buffer_commits > pixels; });
    assert(popup.Open(parent.window, Request()));
    Roundtrip(parent.window);
    popup.Close();
    Roundtrip(parent.window);
    assert(events.closures.size() == 2);
}

struct ReentrantClose {
    WaylandWindow *parent{};
    WaylandPopup *popup{}, *replacement{};
    bool close_parent{};
    unsigned closures{};
    bool retained_display{}, reopened{}, replacement_opened{}, recursive_pump{};

    void Handle(const platform::WaylandPopupEvent &event)
    {
        if (!std::holds_alternative<platform::WaylandPopupClosed>(event)) {
            return;
        }
        ++closures;
        if (close_parent) {
            parent->Close();
            retained_display = parent->Display() && parent->Surface();
            recursive_pump = parent->Pump(0);
        }
        reopened = popup->Open(*parent, Request());
        replacement_opened = replacement->Open(*parent, Request());
    }
};

void CallbackParentClose(Server &server, const std::string &socket)
{
    {
        Parent parent(server, socket);
        WaylandPopup replacement;
        ReentrantClose observer{&parent.window, nullptr, &replacement, true};
        WaylandPopup popup;
        observer.popup = &popup;
        popup.SetEventHandler(std::bind_front(&ReentrantClose::Handle, &observer));
        const auto index = server.Inspect().popups.size();
        assert(popup.Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Configure(index, {20, 44, 160, 80});
        Until(parent.window, [&popup] { return popup.IsConfigured(); });
        Roundtrip(parent.window);
        server.Done(index, true);
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (parent.window.Display()) {
            assert(std::chrono::steady_clock::now() < deadline);
            if (!parent.window.Pump(5)) {
                break;
            }
        }
        assert(!parent.window.Display() && popup.IsClosed());
        assert(observer.closures == 1 && observer.retained_display && !observer.reopened &&
               !observer.replacement_opened && !observer.recursive_pump);
        assert(replacement.IsClosed());
    }
    {
        Parent parent(server, socket);
        WaylandPopup replacement;
        ReentrantClose observer{&parent.window, nullptr, &replacement, false};
        WaylandPopup popup;
        observer.popup = &popup;
        popup.SetEventHandler(std::bind_front(&ReentrantClose::Handle, &observer));
        assert(popup.Open(parent.window, Request()));
        Roundtrip(parent.window);
        parent.window.Close();
        assert(observer.closures == 1 && !observer.reopened && !observer.replacement_opened);
        assert(popup.IsClosed() && replacement.IsClosed() && !parent.window.Display());
    }
}

struct ReopenOnDismissal {
    WaylandWindow *parent{};
    WaylandPopup *popup{};
    bool armed{true}, reopened{};
    unsigned configurations{}, closures{};

    void Handle(const platform::WaylandPopupEvent &event)
    {
        if (std::holds_alternative<platform::WaylandPopupConfigure>(event)) {
            ++configurations;
            return;
        }
        ++closures;
        if (armed) {
            armed = false;
            reopened = popup->Open(*parent, Request());
        }
    }
};

struct ThrowOnConfigure {
    unsigned configurations{};
    std::vector<WaylandPopupCloseReason> closures;

    void Handle(const platform::WaylandPopupEvent &event)
    {
        if (std::holds_alternative<platform::WaylandPopupConfigure>(event)) {
            ++configurations;
            throw std::runtime_error("popup callback fixture failure");
        }
        if (const auto *closed = std::get_if<platform::WaylandPopupClosed>(&event)) {
            closures.push_back(closed->reason);
        }
    }
};

struct DestroyOnDismissal {
    std::unique_ptr<WaylandPopup> *popup{};
    unsigned closures{};

    void Handle(const platform::WaylandPopupEvent &event)
    {
        if (std::holds_alternative<platform::WaylandPopupClosed>(event)) {
            ++closures;
            popup->reset();
        }
    }
};

void CallbackLifetimes(Server &server, const std::string &socket)
{
    Parent parent(server, socket);
    {
        ReopenOnDismissal observer{&parent.window};
        WaylandPopup popup;
        observer.popup = &popup;
        popup.SetEventHandler(std::bind_front(&ReopenOnDismissal::Handle, &observer));
        const auto old_index = server.Inspect().popups.size();
        assert(popup.Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Configure(old_index, {20, 44, 160, 80});
        Until(parent.window, [&popup] { return popup.IsConfigured(); });
        const auto generation = popup.Configure()->generation;
        Roundtrip(parent.window);
        server.Done(old_index, true);
        Until(parent.window, [&observer] { return observer.reopened; });
        assert(!popup.IsClosed() && !popup.IsConfigured() && !popup.Surface());
        Roundtrip(parent.window);
        assert(observer.configurations == 1 && observer.closures == 1);
        const auto index = server.Inspect().popups.size() - 1;
        assert(index > old_index);
        server.Configure(index, {30, 55, 120, 60});
        Until(parent.window, [&popup] { return popup.IsConfigured(); });
        assert(popup.Configure()->generation > generation && observer.configurations == 2);
        popup.Close();
        Roundtrip(parent.window);
        assert(observer.closures == 2);
        CheckDestroyed(server.Inspect().popups[old_index]);
        CheckDestroyed(server.Inspect().popups[index]);
    }
    {
        ThrowOnConfigure observer;
        WaylandPopup popup;
        popup.SetEventHandler(std::bind_front(&ThrowOnConfigure::Handle, &observer));
        const auto index = server.Inspect().popups.size();
        assert(popup.Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Configure(index, {20, 44, 160, 80});
        Until(parent.window, [&popup] { return popup.IsClosed(); });
        Roundtrip(parent.window);
        assert(observer.configurations == 1 && observer.closures.size() == 1 &&
               observer.closures[0] == WaylandPopupCloseReason::ProtocolFailure);
        assert(!popup.Surface() && parent.window.IsMapped());
        CheckDestroyed(server.Inspect().popups[index]);
    }
    {
        std::unique_ptr<WaylandPopup> popup;
        DestroyOnDismissal observer{&popup};
        popup = std::make_unique<WaylandPopup>();
        popup->SetEventHandler(std::bind_front(&DestroyOnDismissal::Handle, &observer));
        const auto index = server.Inspect().popups.size();
        assert(popup->Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Done(index, true);
        Until(parent.window, [&popup] { return !popup; });
        Roundtrip(parent.window);
        assert(observer.closures == 1 && parent.window.IsMapped());
        CheckDestroyed(server.Inspect().popups[index]);
    }
    {
        PopupEvents observer{&parent.window};
        auto popup = std::make_unique<WaylandPopup>();
        popup->SetEventHandler(std::bind_front(&PopupEvents::Handle, &observer));
        const auto index = server.Inspect().popups.size();
        assert(popup->Open(parent.window, Request()));
        Roundtrip(parent.window);
        popup.reset();
        Roundtrip(parent.window);
        assert(observer.closures.empty()); // Destructors release proxies without owner callbacks.
        CheckDestroyed(server.Inspect().popups[index]);
    }
}

void InvalidConfigure(Server &server, const std::string &socket)
{
    Parent parent(server, socket);
    for (int width : {0, 8193}) {
        PopupEvents observer{&parent.window};
        WaylandPopup popup;
        popup.SetEventHandler(std::bind_front(&PopupEvents::Handle, &observer));
        const auto index = server.Inspect().popups.size();
        assert(popup.Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Configure(index, {20, 44, double(width), 80});
        Until(parent.window, [&popup] { return popup.IsClosed(); });
        Roundtrip(parent.window);
        assert(observer.configurations.empty() && observer.closures.size() == 1 &&
               observer.closures[0] == WaylandPopupCloseReason::ProtocolFailure);
        assert(!popup.Configure() && !popup.Surface() && parent.window.IsMapped());
        const auto state = server.Inspect();
        assert(state.popups[index].acks == 0 && state.popups[index].attachments == 0);
        CheckDestroyed(state.popups[index]);
    }
}

void ParentCloseAndProtocolFailure(Server &server, const std::string &socket)
{
    {
        Parent parent(server, socket);
        PopupEvents events{&parent.window};
        auto popup = std::make_unique<WaylandPopup>();
        popup->SetEventHandler(std::bind_front(&PopupEvents::Handle, &events));
        const auto index = server.Inspect().popups.size();
        assert(popup->Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Configure(index, {12, 31, 160, 80});
        Until(parent.window, [&popup] { return popup->IsConfigured(); });
        parent.window.Close();
        assert(popup->IsClosed() && !popup->IsConfigured() && !popup->Surface());
        assert(!popup->AttachBuffer(nullptr, 160, 80));
        assert(events.closures.size() == 1 &&
               events.closures[0] == WaylandPopupCloseReason::ParentClosed &&
               events.parent_alive_at_close);
        popup->Close();
        popup.reset(); // External child may safely outlive the parent's connection.
        assert(events.closures.size() == 1 && !parent.window.Display());
    }
    {
        Parent parent(server, socket);
        PopupEvents events{&parent.window};
        WaylandPopup popup;
        popup.SetEventHandler(std::bind_front(&PopupEvents::Handle, &events));
        const auto index = server.Inspect().popups.size();
        assert(popup.Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Fault(index);
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (parent.window.Pump(5)) {
            assert(std::chrono::steady_clock::now() < deadline);
        }
        assert(popup.IsClosed() && !popup.IsConfigured() && !popup.Surface());
        assert(events.closures.size() == 1 &&
               events.closures[0] == WaylandPopupCloseReason::ProtocolFailure);
        popup.Close();
        assert(events.closures.size() == 1);
    }
}

enum class CloseCheckpoint { WindowCallback, ParentBackend, ChildBackend, ClosedObserver };

struct BeforeCloseHooks {
    BeforeCloseHooks(WaylandWindow &parent, WaylandPopup &child, bool parent_hook = true)
        : parent(parent), child(child), display(parent.Display()), parent_surface(parent.Surface()),
          child_surface(child.Surface()), expect_parent_hook(parent_hook)
    {
        assert(display && parent_surface && child_surface && child.IsConfigured());
        parent_id = wl_proxy_get_id(reinterpret_cast<wl_proxy *>(parent_surface));
        child_id = wl_proxy_get_id(reinterpret_cast<wl_proxy *>(child_surface));
        assert(parent_id && child_id && parent_id != child_id);
        if (parent_hook) {
            parent.SetBeforeCloseHandler(std::bind_front(&BeforeCloseHooks::ParentBackend, this));
        }
        child.SetBeforeCloseHandler(std::bind_front(&BeforeCloseHooks::ChildBackend, this));
        child.SetEventHandler(std::bind_front(&BeforeCloseHooks::PopupEvent, this));
    }

    void Record(CloseCheckpoint checkpoint) noexcept
    {
        assert(checkpoint_count < checkpoints.size());
        checkpoints[checkpoint_count++] = checkpoint;
    }

    void CheckParentProxy() const noexcept
    {
        assert(parent.Display() == display && parent.Surface() == parent_surface);
        auto *proxy = reinterpret_cast<wl_proxy *>(parent_surface);
        assert(wl_proxy_get_id(proxy) == parent_id && wl_proxy_get_display(proxy) == display);
        assert(std::strcmp(wl_proxy_get_class(proxy), "wl_surface") == 0);
    }

    void ParentBackend() noexcept
    {
        assert(++parent_hooks == 1);
        Record(CloseCheckpoint::ParentBackend);
        CheckParentProxy();
        assert(!child.IsClosed() && child.IsConfigured() && child.Surface() == child_surface);
        assert(child_hooks == 0 && closed_events == 0);
    }

    void ChildBackend() noexcept
    {
        assert(++child_hooks == 1);
        Record(CloseCheckpoint::ChildBackend);
        CheckParentProxy();
        assert(child.IsClosed() && !child.IsConfigured() && !child.Surface());
        assert(!closed_events && parent_hooks == (expect_parent_hook ? 1u : 0u));
        auto *proxy = reinterpret_cast<wl_proxy *>(child_surface);
        assert(wl_proxy_get_id(proxy) == child_id && wl_proxy_get_display(proxy) == display);
        assert(std::strcmp(wl_proxy_get_class(proxy), "wl_surface") == 0);
    }

    void PopupEvent(const platform::WaylandPopupEvent &event) noexcept
    {
        if (const auto *closed = std::get_if<platform::WaylandPopupClosed>(&event)) {
            assert(++closed_events == 1 && child_hooks == 1);
            Record(CloseCheckpoint::ClosedObserver);
            reason = closed->reason;
            assert(child.IsClosed() && !child.IsConfigured() && !child.Surface());
            CheckParentProxy();
        }
    }

    void WindowEvent(const contracts::WindowEvent &event)
    {
        if (!std::holds_alternative<contracts::PointerButtonEvent>(event) || window_callbacks) {
            return;
        }
        ++window_callbacks;
        Record(CloseCheckpoint::WindowCallback);
        parent.Close();
        CheckParentProxy(); // The connection survives until Pump leaves native dispatch.
        assert(parent_hooks == 1 && child_hooks == 1 && closed_events == 1);
        retained_display = true;
        assert(!parent.Pump(0));
        parent.Close();
        assert(parent_hooks == 1 && child_hooks == 1 && closed_events == 1);
    }

    void CheckParentClose(bool dispatch_callback, WaylandPopupCloseReason expected) const
    {
        assert(!parent.Display() && !parent.Surface());
        assert(child.IsClosed() && !child.IsConfigured() && !child.Surface());
        assert(parent_hooks == 1 && child_hooks == 1 && closed_events == 1 && reason == expected);
        const auto offset = dispatch_callback ? 1u : 0u;
        assert(checkpoint_count == offset + 3);
        assert(checkpoints[offset] == CloseCheckpoint::ParentBackend);
        assert(checkpoints[offset + 1] == CloseCheckpoint::ChildBackend);
        assert(checkpoints[offset + 2] == CloseCheckpoint::ClosedObserver);
        if (dispatch_callback) {
            assert(window_callbacks == 1 && retained_display);
            assert(checkpoints[0] == CloseCheckpoint::WindowCallback);
        }
    }

    WaylandWindow &parent;
    WaylandPopup &child;
    wl_display *display{};
    wl_surface *parent_surface{}, *child_surface{};
    std::uint32_t parent_id{}, child_id{};
    bool expect_parent_hook{}, retained_display{};
    unsigned parent_hooks{}, child_hooks{}, closed_events{}, window_callbacks{};
    WaylandPopupCloseReason reason{WaylandPopupCloseReason::Requested};
    std::array<CloseCheckpoint, 8> checkpoints{};
    std::size_t checkpoint_count{};
};

void BeforeCloseBackendHooks(Server &server, const std::string &socket)
{
    for (const bool dispatch_callback : {false, true}) {
        Parent parent(server, socket);
        WaylandPopup popup;
        const auto index = server.Inspect().popups.size();
        assert(popup.Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Configure(index, {12, 31, 160, 80});
        Until(parent.window, [&popup] { return popup.IsConfigured(); });
        Roundtrip(parent.window);
        BeforeCloseHooks observer(parent.window, popup);
        if (dispatch_callback) {
            parent.window.SetEventHandler(
                std::bind_front(&BeforeCloseHooks::WindowEvent, &observer));
            server.Input(false);
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (parent.window.Display()) {
                assert(std::chrono::steady_clock::now() < deadline);
                if (!parent.window.Pump(5)) {
                    break;
                }
            }
        } else {
            parent.window.Close();
        }
        observer.CheckParentClose(dispatch_callback, WaylandPopupCloseReason::ParentClosed);
        const auto checkpoints = observer.checkpoint_count;
        parent.window.Close();
        popup.Close();
        assert(observer.checkpoint_count == checkpoints);
        parent.window.SetEventHandler({});
    }
    {
        Parent parent(server, socket);
        WaylandPopup popup;
        const auto index = server.Inspect().popups.size();
        assert(popup.Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Configure(index, {12, 31, 160, 80});
        Until(parent.window, [&popup] { return popup.IsConfigured(); });
        Roundtrip(parent.window);
        BeforeCloseHooks observer(parent.window, popup);
        server.Fault(index);
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (parent.window.Pump(5)) {
            assert(std::chrono::steady_clock::now() < deadline);
        }
        observer.CheckParentClose(false, WaylandPopupCloseReason::ProtocolFailure);
        const auto checkpoints = observer.checkpoint_count;
        parent.window.Close();
        popup.Close();
        assert(observer.checkpoint_count == checkpoints);
    }
    {
        Parent parent(server, socket);
        WaylandPopup popup;
        const auto index = server.Inspect().popups.size();
        assert(popup.Open(parent.window, Request()));
        Roundtrip(parent.window);
        server.Configure(index, {12, 31, 160, 80});
        Until(parent.window, [&popup] { return popup.IsConfigured(); });
        Roundtrip(parent.window);
        BeforeCloseHooks observer(parent.window, popup, false);
        server.Done(index);
        Until(parent.window, [&popup] { return popup.IsClosed(); });
        Roundtrip(parent.window);
        assert(observer.parent_hooks == 0 && observer.child_hooks == 1 &&
               observer.closed_events == 1);
        assert(observer.reason == WaylandPopupCloseReason::CompositorDismissed);
        assert(observer.checkpoint_count == 2 &&
               observer.checkpoints[0] == CloseCheckpoint::ChildBackend &&
               observer.checkpoints[1] == CloseCheckpoint::ClosedObserver);
        CheckDestroyed(server.Inspect().popups[index]);
        popup.Close();
        assert(observer.checkpoint_count == 2 && parent.window.IsMapped());
    }
}

struct ChildObserver {
    std::vector<platform::WaylandPopupInput> inputs;
    std::vector<platform::WaylandPopupPresentation> presentations;
    std::vector<platform::WaylandPopupFrameReady> frames;
    std::vector<platform::WaylandPopupClosed> closures;

    void Input(const platform::WaylandPopupInput &event)
    {
        inputs.push_back(event);
    }

    void Presentation(const platform::WaylandPopupPresentation &event)
    {
        presentations.push_back(event);
    }

    void Event(const platform::WaylandPopupEvent &event)
    {
        if (const auto *frame = std::get_if<platform::WaylandPopupFrameReady>(&event)) {
            frames.push_back(*frame);
        } else if (const auto *closed = std::get_if<platform::WaylandPopupClosed>(&event)) {
            closures.push_back(*closed);
        }
    }

    template <typename Event> unsigned Count() const
    {
        return std::count_if(inputs.begin(), inputs.end(), [](const auto &event) {
            return std::holds_alternative<Event>(event.event);
        });
    }
};

struct ChildCommit {
    WaylandPopup *popup{};
    wl_buffer *buffer{};
    int width{160}, height{80};
    unsigned calls{};
    bool fail{};

    bool Commit()
    {
        ++calls;
        if (fail) {
            return false;
        }
        wl_surface_attach(popup->Surface(), buffer, 0, 0);
        wl_surface_damage_buffer(popup->Surface(), 0, 0, width, height);
        wl_surface_commit(popup->Surface());
        return true;
    }
};

void IndependentChildScheduling(Server &server, const std::string &socket)
{
    Parent parent(server, socket);
    Buffers buffers(parent.window);
    WaylandPopup popup;
    ChildObserver observer;
    popup.SetEventHandler(std::bind_front(&ChildObserver::Event, &observer));
    popup.SetInputHandler(std::bind_front(&ChildObserver::Input, &observer));
    popup.SetPresentationHandler(std::bind_front(&ChildObserver::Presentation, &observer));
    const auto index = server.Inspect().popups.size();
    assert(popup.Open(parent.window, Request()));
    Roundtrip(parent.window);
    server.Configure(index, {20, 44, 160, 80});
    Until(parent.window, [&popup] { return popup.IsConfigured(); });
    const auto target = popup.Target();
    assert(target && target.surface_lifetime_id && target.configure_generation);
    assert(popup.SetBufferLayout(*popup.BufferLayout()));
    const contracts::SurfaceInputRegion mask[]{{{8, 10, 144, 60}, 6}};
    assert(popup.SetInputRegions(target, mask));
    Roundtrip(parent.window);
    const auto staged = server.Inspect().popups[index];
    assert(staged.input_requests == 1 && staged.buffer_commits == 0);
    assert(popup.SubmitState(target) == platform::SubmitResult::AwaitFrame);
    Roundtrip(parent.window);
    assert(server.Inspect().popups[index].empty_commits == staged.empty_commits);
    assert(popup.SetInputRegions(target, mask));
    assert(!popup.SetInputRegions({target.surface_lifetime_id + 1, target.configure_generation},
                                  mask));
    Roundtrip(parent.window);
    assert(server.Inspect().popups[index].input_requests == 1);
    std::vector<contracts::SurfaceInputRegion> spans;
    for (unsigned row = 0; row < 80; ++row) {
        spans.push_back({{8 + double(row % 5), double(row), 140, 1}, 0});
    }
    assert(popup.SetInputRegions(target, spans));
    assert(popup.SetInputRegions(target, mask));
    const std::vector<contracts::SurfaceInputRegion> excessive(4097, mask[0]);
    assert(!popup.SetInputRegions(target, excessive));
    Roundtrip(parent.window);
    assert(server.Inspect().popups[index].input_requests == 3);

    // Native focus before a compatible pixel commit grants no child input scope.
    parent.events.events.clear();
    server.Input(true, index);
    Roundtrip(parent.window);
    assert(observer.inputs.empty() && parent.events.events.empty());

    ChildCommit commit{&popup, buffers.Create(160, 80)};
    const auto submit = std::bind_front(&ChildCommit::Commit, &commit);
    server.HoldFrames(true);
    assert(popup.SubmitPixels(target, submit) == platform::SubmitResult::Pixels);
    const auto first = popup.LastPixelSubmission();
    assert(first && popup.FrameCallbackPending() && popup.PresentationPending(first));
    assert(popup.SubmitPixels(target, submit) == platform::SubmitResult::AwaitFrame);
    assert(commit.calls == 1);
    Roundtrip(parent.window);
    const auto pixel_wire = server.Inspect().popups[index];
    const contracts::SurfaceInputRegion updated_mask[]{{{12, 10, 136, 60}, 6}};
    assert(popup.SetInputRegions(target, updated_mask));
    assert(popup.SubmitState(target) == platform::SubmitResult::State);
    assert(popup.SubmitState(target) == platform::SubmitResult::None);
    assert(popup.FrameCallbackPending() && popup.LastPixelSubmission() == first);
    Roundtrip(parent.window);
    const auto state_wire = server.Inspect().popups[index];
    assert(state_wire.empty_commits == pixel_wire.empty_commits + 1 &&
           state_wire.frame_requests == pixel_wire.frame_requests &&
           state_wire.feedback_requests == pixel_wire.feedback_requests);
    const auto root_submission = parent.window.LastPixelSubmission();
    parent.window.RequestRedraw();
    Until(parent.window, [&parent, root_submission] {
        return parent.window.LastPixelSubmission().value > root_submission.value;
    });
    assert(popup.FrameCallbackPending() && popup.LastPixelSubmission() == first);
    assert(server.Inspect().popups[index].buffer_commits == 1);

    server.ReleaseFrames(index);
    Until(parent.window, [&popup] { return !popup.FrameCallbackPending(); });
    assert(observer.frames.size() == 1 && observer.frames[0].target == target &&
           observer.frames[0].submission == first);
    assert(popup.SubmitPixels(target, submit) == platform::SubmitResult::Pixels);
    const auto second = popup.LastPixelSubmission();
    Roundtrip(parent.window);
    assert(second.value == first.value + 1 && popup.FrameCallbackPending());
    server.ReleaseFeedback(index, 0, true);
    Until(parent.window, [&observer] { return observer.presentations.size() == 1; });
    assert(observer.presentations[0].target == target &&
           observer.presentations[0].presentation.submission == first &&
           observer.presentations[0].presentation.outcome ==
               platform::PresentationOutcome::Discarded);
    assert(popup.FrameCallbackPending()); // Old discard cannot retire a newer callback.
    server.ReleaseFeedback(index, 0);
    Until(parent.window, [&observer] { return observer.presentations.size() == 2; });
    assert(observer.presentations[1].presentation.submission == second &&
           observer.presentations[1].presentation.outcome ==
               platform::PresentationOutcome::Presented);
    server.ReleaseFrames(index);
    Until(parent.window, [&popup] { return !popup.FrameCallbackPending(); });
    assert(!popup.PresentationPending(first) && !popup.PresentationPending(second));

    server.HoldFrames(false);
    for (unsigned slot = 0; slot < 8; ++slot) {
        assert(popup.SubmitPixels(target, submit) == platform::SubmitResult::Pixels);
        Until(parent.window, [&popup] { return !popup.FrameCallbackPending(); });
    }
    const auto calls = commit.calls;
    assert(popup.SubmitPixels(target, submit) == platform::SubmitResult::AwaitFrame);
    assert(commit.calls == calls && !popup.FrameCallbackPending());
    server.ReleaseFeedback(index, 3);
    Until(parent.window, [&observer] { return observer.presentations.size() == 3; });
    assert(popup.SubmitPixels(target, submit) == platform::SubmitResult::Pixels);
    Until(parent.window, [&popup] { return !popup.FrameCallbackPending(); });

    parent.events.events.clear();
    server.Input(true, index);
    Roundtrip(parent.window);
    assert(parent.events.events.empty());
    assert(observer.Count<contracts::PointerEnterEvent>() == 1);
    assert(observer.Count<contracts::PointerButtonEvent>() == 2);
    assert(observer.Count<contracts::KeyEvent>() == 2);
    assert(observer.Count<contracts::FocusEvent>() == 2);
    assert(observer.Count<contracts::TouchDownEvent>() == 0);
    for (const auto &input : observer.inputs) {
        assert(input.target == target && input.submission == popup.LastPixelSubmission());
        if (const auto *button = std::get_if<contracts::PointerButtonEvent>(&input.event)) {
            assert(button->position == contracts::LogicalPoint(15, 16) && button->protocol_serial);
        }
    }
    server.FocusChild(index);
    Roundtrip(parent.window);
    assert(parent.events.Count<contracts::FocusEvent>() == 1);
    const auto root_leave = std::get<contracts::FocusEvent>(parent.events.events.back());
    assert(!root_leave.focused && root_leave.internal_transfer);
    const auto old_submission = popup.LastPixelSubmission();
    const auto presentation_count = observer.presentations.size();
    server.Configure(index, {30, 50, 120, 60});
    Until(parent.window, [&popup, target] { return popup.Target() != target; });
    assert(observer.presentations.size() == presentation_count + 8);
    assert(observer.Count<contracts::PointerCancelEvent>() == 1);
    const auto &cancel =
        *std::find_if(observer.inputs.begin(), observer.inputs.end(), [](const auto &event) {
            return std::holds_alternative<contracts::PointerCancelEvent>(event.event);
        });
    assert(cancel.target == target && cancel.submission == old_submission);
    const auto &retired_focus = observer.inputs.back();
    assert(retired_focus.target == target && retired_focus.submission == old_submission);
    const auto focus_cancel = std::get<contracts::FocusEvent>(retired_focus.event);
    assert(!focus_cancel.focused && focus_cancel.internal_transfer);
    const auto new_target = popup.Target();
    assert(new_target.surface_lifetime_id == target.surface_lifetime_id &&
           new_target.configure_generation > target.configure_generation);
    assert(popup.SubmitPixels(target, submit) == platform::SubmitResult::None);
    assert(popup.SubmitState(target) == platform::SubmitResult::AwaitFrame);
    assert(popup.SubmitState(new_target) == platform::SubmitResult::AwaitFrame);
    assert(commit.calls == calls + 1);
    const auto inputs = observer.inputs.size();
    server.Input(true, index);
    Roundtrip(parent.window);
    assert(observer.inputs.size() == inputs);
    assert(popup.SetBufferLayout(*popup.BufferLayout()));
    commit.buffer = buffers.Create(120, 60);
    commit.width = 120;
    commit.height = 60;
    assert(popup.SubmitPixels(new_target, submit) == platform::SubmitResult::Pixels);
    Until(parent.window, [&popup] { return !popup.FrameCallbackPending(); });
    server.FocusChild(index);
    Roundtrip(parent.window);
    popup.Close();
    assert(observer.closures.size() == 1 && observer.closures[0].target == new_target);
    assert(!popup.FrameCallbackPending() && !popup.Target());
    assert(observer.Count<contracts::PointerCancelEvent>() == 2);
    assert(popup.SubmitPixels(new_target, submit) == platform::SubmitResult::None);
    Roundtrip(parent.window);
    assert(parent.window.IsMapped());

    const auto reopened_index = server.Inspect().popups.size();
    assert(popup.Open(parent.window, Request()));
    Roundtrip(parent.window);
    server.Configure(reopened_index, {20, 44, 160, 80});
    Until(parent.window, [&popup] { return popup.IsConfigured(); });
    assert(popup.Target().surface_lifetime_id != target.surface_lifetime_id);
    assert(popup.SetBufferLayout(*popup.BufferLayout()));
    commit.fail = true;
    const auto feedbacks_before = observer.presentations.size();
    assert(popup.SubmitPixels(popup.Target(), submit) == platform::SubmitResult::Failed);
    assert(popup.IsClosed() && !popup.FrameCallbackPending());
    assert(observer.closures.size() == 2 &&
           observer.closures.back().reason == WaylandPopupCloseReason::ProtocolFailure);
    assert(observer.presentations.size() == feedbacks_before);
    Roundtrip(parent.window);
    assert(parent.window.IsMapped());
}

struct DestroyOnInputCancellation {
    std::unique_ptr<WaylandPopup> *popup{};
    unsigned cancellations{};

    void Handle(const platform::WaylandPopupInput &input)
    {
        if (std::holds_alternative<contracts::PointerCancelEvent>(input.event)) {
            ++cancellations;
            popup->reset();
        }
    }
};

void CancellationRetiresBeforeNotification(Server &server, const std::string &socket)
{
    Parent parent(server, socket);
    Buffers buffers(parent.window);
    std::unique_ptr<WaylandPopup> popup = std::make_unique<WaylandPopup>();
    DestroyOnInputCancellation observer{&popup};
    popup->SetInputHandler(std::bind_front(&DestroyOnInputCancellation::Handle, &observer));
    const auto index = server.Inspect().popups.size();
    assert(popup->Open(parent.window, Request()));
    Roundtrip(parent.window);
    server.Configure(index, {20, 44, 160, 80});
    Until(parent.window, [&popup] { return popup->IsConfigured(); });
    auto *buffer = buffers.Create(160, 80);
    assert(popup->AttachBuffer(buffer, 160, 80));
    Roundtrip(parent.window);
    server.FocusChild(index);
    Roundtrip(parent.window);
    popup->Close();
    assert(!popup && observer.cancellations == 1);
    Roundtrip(parent.window);
    CheckDestroyed(server.Inspect().popups[index]);
    assert(parent.window.IsMapped());
    buffers.Close();
    parent.window.Close(); // No dangling popup registration survives its observer.
    assert(!parent.window.Display());
}

void ChildDeviceRemoval(Server &server, const std::string &socket)
{
    Parent parent(server, socket);
    Buffers buffers(parent.window);
    ChildObserver observer;
    WaylandPopup popup;
    popup.SetInputHandler(std::bind_front(&ChildObserver::Input, &observer));
    const auto index = server.Inspect().popups.size();
    assert(popup.Open(parent.window, Request()));
    Roundtrip(parent.window);
    server.Configure(index, {20, 44, 160, 80});
    Until(parent.window, [&popup] { return popup.IsConfigured(); });
    auto *buffer = buffers.Create(160, 80);
    assert(popup.AttachBuffer(buffer, 160, 80));
    Roundtrip(parent.window);
    server.FocusChild(index);
    Roundtrip(parent.window);
    const auto target = popup.Target();
    const auto submission = popup.LastPixelSubmission();
    observer.inputs.clear();
    parent.events.events.clear();
    server.RemoveDevices();
    Roundtrip(parent.window);
    assert(observer.Count<contracts::PointerCancelEvent>() == 1 &&
           observer.Count<contracts::FocusEvent>() == 1);
    for (const auto &input : observer.inputs) {
        assert(input.target == target && input.submission == submission);
        if (const auto *focus = std::get_if<contracts::FocusEvent>(&input.event)) {
            assert(!focus->focused && !focus->internal_transfer);
        }
    }
    assert(parent.events.Count<contracts::PointerCancelEvent>() == 0 &&
           parent.events.Count<contracts::FocusEvent>() == 0);
    popup.Close();
    Roundtrip(parent.window);
}

contracts::SurfaceEffectRegion PopupEffectContour(double neck_offset = 0)
{
    contracts::Contour contour{{{48 + neck_offset, 10},
                                {64 + neck_offset, 10},
                                {64 + neck_offset, 18},
                                {156, 18},
                                {156, 82},
                                {20, 82},
                                {20, 18},
                                {48 + neck_offset, 18}}};
    return {contracts::ContourBounds(contour), 0, 8, std::move(contour)};
}

void RejectedPopupEffects(Server &server, WaylandWindow &parent, WaylandPopup &popup,
                          platform::WaylandPopupTarget target,
                          std::span<const contracts::SurfaceEffectRegion> effects)
{
    // IsConfigured only proves receipt/ACK enqueue. Drain earlier legitimate
    // requests before the snapshot, especially the ACK of a reopened child.
    Roundtrip(parent);
    const auto before = server.Inspect();
    assert(!popup.SetSurfaceEffects(target, effects));
    Roundtrip(parent);
    assert(server.Inspect() == before);
}

struct FlushChildRetirement {
    WaylandWindow *parent{};
    bool parent_closed{};

    void Handle(const platform::WaylandPopupEvent &event)
    {
        const auto *closed = std::get_if<platform::WaylandPopupClosed>(&event);
        if (!closed || closed->reason != WaylandPopupCloseReason::ParentClosed) {
            return;
        }
        assert(parent && parent->Display() && parent->Surface());
        // Parent Close will disconnect without a roundtrip. Deliver this
        // child's queued destroys before testing their protocol order, rather
        // than observing unordered server cleanup after client disconnect.
        assert(wl_display_flush(parent->Display()) >= 0);
        parent_closed = true;
    }
};

void ChildSurfaceEffects(const std::string &socket, unsigned version, bool popup_backdrop,
                         bool contour, bool backdrop)
{
    Server server(socket, true, version, popup_backdrop, contour, backdrop);
    Parent parent(server, socket);
    const platform::WaylandSurfaceEffectCapabilities expected{
        version > 0 && backdrop, version >= 2 && contour, version >= 3 && popup_backdrop};
    assert(parent.window.SurfaceEffectCapabilities() == expected);

    Buffers buffers(parent.window);
    WaylandPopup popup;
    assert(popup.SurfaceEffectCapabilities() == platform::WaylandSurfaceEffectCapabilities{});
    const auto index = server.Inspect().popups.size();
    assert(popup.Open(parent.window, Request()));
    assert(popup.SurfaceEffectCapabilities() == expected);
    assert(!popup.SetSurfaceEffects({}, {}));
    Roundtrip(parent.window);
    server.Configure(index, {20, 44, 160, 80});
    Until(parent.window, [&popup] { return popup.IsConfigured(); });
    const auto target = popup.Target();
    assert(popup.SetBufferLayout({{188, 108}, {12, 10, 160, 80}, target.configure_generation}));
    Roundtrip(parent.window);

    const contracts::SurfaceEffectRegion rectangle{{14.251, 12.75, 140.001, 64}, 8, 8};
    const std::array only_rectangle{rectangle};
    const std::array mixed{rectangle, PopupEffectContour()};
    if (!expected.backdrop || !expected.popup_backdrop) {
        RejectedPopupEffects(server, parent.window, popup, target, only_rectangle);
        RejectedPopupEffects(server, parent.window, popup, target, mixed);
        assert(popup.SetSurfaceEffects(target, {}));
        assert(popup.AttachBuffer(buffers.Create(188, 108), 188, 108));
        Roundtrip(parent.window);
        assert(popup.SubmitState(target) == platform::SubmitResult::None);
        assert(server.Inspect().popups[index].effect_creates == 0);
        popup.Close();
        Roundtrip(parent.window);
        assert(popup.SurfaceEffectCapabilities() == platform::WaylandSurfaceEffectCapabilities{});
        return;
    }

    if (!expected.contour) {
        RejectedPopupEffects(server, parent.window, popup, target, mixed);
    }
    const std::span<const contracts::SurfaceEffectRegion> requested =
        expected.contour ? std::span<const contracts::SurfaceEffectRegion>(mixed)
                         : std::span<const contracts::SurfaceEffectRegion>(only_rectangle);
    assert(popup.SetSurfaceEffects(target, requested));
    Roundtrip(parent.window);
    auto staged = server.Inspect().popups[index];
    assert(staged.effect_creates == 1 && staged.effect_clears == 1 &&
           staged.effect_rectangles == 1 && staged.effect_contours == unsigned(expected.contour));
    assert(staged.pending_effects.size() == requested.size() && staged.current_effects.empty());
    assert(staged.pending_effects.front().bounds.x ==
           wl_fixed_to_double(wl_fixed_from_double(rectangle.bounds.x)));
    assert(popup.SubmitState(target) == platform::SubmitResult::AwaitFrame);
    Roundtrip(parent.window);
    assert(server.Inspect().popups[index] == staged);

    server.HoldFrames(true);
    assert(popup.AttachBuffer(buffers.Create(188, 108), 188, 108));
    Roundtrip(parent.window);
    auto pixels = server.Inspect().popups[index];
    assert(pixels.current_effects == pixels.pending_effects && pixels.effect_commits == 1);
    assert(pixels.effect_request_order < pixels.commit_order);
    const auto submission = popup.LastPixelSubmission();
    assert(submission && popup.FrameCallbackPending());
    assert(popup.SetSurfaceEffects(target, requested));
    assert(popup.SubmitState(target) == platform::SubmitResult::None);
    Roundtrip(parent.window);
    assert(server.Inspect().popups[index] == pixels);

    auto invalid_rectangle = rectangle;
    invalid_rectangle.bounds.width = -1;
    const std::array invalid_mixed{rectangle, invalid_rectangle};
    RejectedPopupEffects(server, parent.window, popup, target, invalid_mixed);
    auto tiny = rectangle;
    tiny.bounds.width = 0.001;
    const std::array invalid_fixed{rectangle, tiny};
    RejectedPopupEffects(server, parent.window, popup, target, invalid_fixed);
    const std::vector<contracts::SurfaceEffectRegion> excessive(9, rectangle);
    RejectedPopupEffects(server, parent.window, popup, target, excessive);
    RejectedPopupEffects(server, parent.window, popup,
                         {target.surface_lifetime_id + 1, target.configure_generation}, requested);
    RejectedPopupEffects(server, parent.window, popup,
                         {target.surface_lifetime_id, target.configure_generation + 1}, requested);
    if (expected.contour) {
        auto invalid_contour = PopupEffectContour();
        invalid_contour.contour->points[1] = invalid_contour.contour->points[0];
        const std::array invalid_geometry{rectangle, invalid_contour};
        RejectedPopupEffects(server, parent.window, popup, target, invalid_geometry);
    }

    auto changed = rectangle;
    changed.blur_radius = 11;
    const std::array changed_rectangle{changed};
    assert(popup.SetSurfaceEffects(target, changed_rectangle));
    Roundtrip(parent.window);
    staged = server.Inspect().popups[index];
    assert(staged.pending_effects != pixels.current_effects &&
           staged.current_effects == pixels.current_effects);
    assert(popup.SubmitState(target) == platform::SubmitResult::State);
    assert(popup.LastPixelSubmission() == submission && popup.FrameCallbackPending());
    assert(popup.SubmitState(target) == platform::SubmitResult::None);
    Roundtrip(parent.window);
    const auto metadata = server.Inspect().popups[index];
    assert(metadata.current_effects == metadata.pending_effects &&
           metadata.empty_commits == pixels.empty_commits + 1 &&
           metadata.frame_requests == pixels.frame_requests &&
           metadata.feedback_requests == pixels.feedback_requests && metadata.buffer_commits == 1);
    if (expected.contour) {
        const std::array changed_contour{rectangle, PopupEffectContour(4)};
        assert(changed_contour[1].bounds == mixed[1].bounds);
        assert(popup.SetSurfaceEffects(target, changed_contour));
        assert(popup.SubmitState(target) == platform::SubmitResult::State);
        Roundtrip(parent.window);
        const auto shape_changed = server.Inspect().popups[index];
        assert(shape_changed.current_effects.back() == changed_contour.back());
        assert(contracts::DecodeContour(shape_changed.last_effect_payload) ==
               *changed_contour.back().contour);
    }

    // A configure revokes even an uncommitted descriptor. Old target updates
    // cannot install it into the new pending state or acquire metadata adoption.
    assert(popup.SetSurfaceEffects(target, changed_rectangle));
    Roundtrip(parent.window);
    server.Configure(index, {28, 48, 160, 80});
    Until(parent.window, [&popup, target] { return popup.Target() != target; });
    Roundtrip(parent.window);
    const auto reconfigured = server.Inspect().popups[index];
    assert(reconfigured.pending_effects.empty());
    const auto current_target = popup.Target();
    assert(current_target.surface_lifetime_id == target.surface_lifetime_id &&
           current_target.configure_generation > target.configure_generation);
    RejectedPopupEffects(server, parent.window, popup, target, requested);
    assert(popup.SubmitState(current_target) == platform::SubmitResult::AwaitFrame);
    assert(popup.SetBufferLayout(
        {{188, 108}, {12, 10, 160, 80}, current_target.configure_generation}));
    assert(popup.SetSurfaceEffects(current_target, requested));
    assert(popup.AttachBuffer(buffers.Create(188, 108), 188, 108));
    Roundtrip(parent.window);
    pixels = server.Inspect().popups[index];
    assert(pixels.current_effects == pixels.pending_effects &&
           pixels.current_effects.size() == requested.size() && pixels.buffer_commits == 2);

    assert(popup.SetSurfaceEffects(current_target, {}));
    assert(popup.SubmitState(current_target) == platform::SubmitResult::State);
    Roundtrip(parent.window);
    assert(server.Inspect().popups[index].current_effects.empty());
    popup.Close();
    Roundtrip(parent.window);
    const auto destroyed = server.Inspect().popups[index];
    assert(destroyed.effect_destroy_order &&
           destroyed.effect_destroy_order < destroyed.popup_destroy_order &&
           destroyed.effect_destroy_order < destroyed.surface_destroy_order);
    assert(!popup.SetSurfaceEffects(current_target, requested));
    assert(popup.SurfaceEffectCapabilities() == platform::WaylandSurfaceEffectCapabilities{});

    const auto next_index = server.Inspect().popups.size();
    assert(popup.Open(parent.window, Request()));
    Roundtrip(parent.window);
    server.Configure(next_index, {20, 44, 160, 80});
    Until(parent.window, [&popup] { return popup.IsConfigured(); });
    assert(popup.Target().surface_lifetime_id != target.surface_lifetime_id);
    RejectedPopupEffects(server, parent.window, popup, current_target, requested);
    assert(popup.SetBufferLayout(*popup.BufferLayout()));
    assert(popup.SetSurfaceEffects(popup.Target(), only_rectangle));
    assert(popup.AttachBuffer(buffers.Create(160, 80), 160, 80));
    Roundtrip(parent.window);
    const auto before_parent_close = server.Inspect().popups[next_index];
    assert(before_parent_close.effect_creates == 1 && before_parent_close.effect_commits == 1 &&
           !before_parent_close.current_effects.empty());
    FlushChildRetirement flush{&parent.window};
    popup.SetEventHandler(std::bind_front(&FlushChildRetirement::Handle, &flush));
    buffers.Close();
    parent.window.Close();
    assert(flush.parent_closed && popup.IsClosed());
    const auto parent_closed = server.Inspect().popups[next_index];
    assert(parent_closed.effect_destroy_order &&
           parent_closed.effect_destroy_order < parent_closed.popup_destroy_order &&
           parent_closed.effect_destroy_order < parent_closed.surface_destroy_order);
    assert(parent.window.SurfaceEffectCapabilities() ==
           platform::WaylandSurfaceEffectCapabilities{});
}

} // namespace

int main()
{
    char directory_template[] = "/tmp/prism-popup-client-test.XXXXXX";
    const char *directory = mkdtemp(directory_template);
    assert(directory && setenv("XDG_RUNTIME_DIR", directory, 1) == 0);
    const std::string socket = "wayland-popup-client-" + std::to_string(getpid());
    {
        Server server(socket);
        PositionerAndBufferGuards(server, socket);
        BufferLayouts(server, socket);
        ChildInputAndDismissal(server, socket);
        ParentCommittedGeometry(server, socket);
        CallbackParentClose(server, socket);
        CallbackLifetimes(server, socket);
        InvalidConfigure(server, socket);
        ParentCloseAndProtocolFailure(server, socket);
        BeforeCloseBackendHooks(server, socket);
    }
    {
        const auto scheduled_socket = socket + "-scheduled";
        Server server(scheduled_socket, true);
        IndependentChildScheduling(server, scheduled_socket);
        CancellationRetiresBeforeNotification(server, scheduled_socket);
        ChildDeviceRemoval(server, scheduled_socket);
    }
    ChildSurfaceEffects(socket + "-effects-absent", 0, true, true, true);
    ChildSurfaceEffects(socket + "-effects-v1", 1, true, true, true);
    ChildSurfaceEffects(socket + "-effects-v2", 2, true, true, true);
    ChildSurfaceEffects(socket + "-effects-v3-disabled", 3, false, true, true);
    ChildSurfaceEffects(socket + "-effects-v3-no-backdrop", 3, true, true, false);
    ChildSurfaceEffects(socket + "-effects-v3-rectangle", 3, true, false, true);
    ChildSurfaceEffects(socket + "-effects-v3-contour", 3, true, true, true);
    std::filesystem::remove_all(directory);
    std::puts("Wayland popup: parent positioner, configure/buffer guards, child input isolation, "
              "dismissal and owner teardown passed");
}
