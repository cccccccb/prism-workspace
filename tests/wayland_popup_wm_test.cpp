#include "fixtures/wm_theme_fixture.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/wlr_server.hpp"
#include "xdg-shell-client-protocol.h"

extern "C" {
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {
using namespace prism;

void Require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

enum class Command { None, Popup, Nested, Reposition, Destroy, Fullscreen, Restore, Unmap, Stop };

struct Observations {
    std::atomic<Command> command{};
    std::atomic<bool> ready{}, failed{}, stopped{};
    std::array<std::atomic<unsigned>, 3> ids{}, configured{}, done{}, buttons{}, keys{};
    std::array<std::atomic<int>, 3> x{}, y{}, width{}, height{};
    std::atomic<unsigned> repositioned{};
    std::atomic<int> pointer_x{}, pointer_y{}, focused{-1};
};

struct Buffer {
    wl_buffer *native{};
    void *pixels{MAP_FAILED};
    std::size_t size{};

    ~Buffer()
    {
        if (native) {
            wl_buffer_destroy(native);
        }
        if (pixels != MAP_FAILED) {
            munmap(pixels, size);
        }
    }
};

class Client {
    struct Surface {
        Client *owner{};
        unsigned index{};
        wl_surface *native{};
        xdg_surface *xdg{};
        xdg_toplevel *top{};
        xdg_popup *popup{};
        int x{}, y{}, width{640}, height{400}, inset_x{}, inset_y{};
    };

public:
    Client(std::string socket, Observations &observations, bool explicit_geometry)
        : socket_(std::move(socket)), observations_(observations), explicit_(explicit_geometry)
    {
        for (unsigned i = 0; i < surfaces_.size(); ++i) {
            surfaces_[i].owner = this;
            surfaces_[i].index = i;
        }
    }

    void Run() noexcept
    {
        try {
            Initialize();
            observations_.ready = true;
            while (true) {
                const auto command = observations_.command.exchange(Command::None);
                if (command == Command::Stop) {
                    break;
                }
                ProcessCommand(command);
                if (!Pump()) {
                    throw std::runtime_error("Wayland client transport failed");
                }
            }
        } catch (const std::exception &error) {
            std::fprintf(stderr, "popup client: %s\n", error.what());
            observations_.failed = true;
        }
        Close();
        observations_.stopped = true;
    }

private:
    static void Global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                       std::uint32_t version)
    {
        auto &self = *static_cast<Client *>(data);
        if (std::strcmp(interface, "wl_compositor") == 0) {
            self.compositor_ = static_cast<wl_compositor *>(
                wl_registry_bind(registry, name, &wl_compositor_interface, 4));
        } else if (std::strcmp(interface, "wl_shm") == 0) {
            self.shm_ =
                static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        } else if (std::strcmp(interface, "xdg_wm_base") == 0) {
            self.shell_ = static_cast<xdg_wm_base *>(
                wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 3u)));
            static const xdg_wm_base_listener listener{Ping};
            xdg_wm_base_add_listener(self.shell_, &listener, &self);
        } else if (std::strcmp(interface, "wl_seat") == 0) {
            self.seat_ = static_cast<wl_seat *>(
                wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
            static const wl_seat_listener listener{SeatCapabilities, SeatName};
            wl_seat_add_listener(self.seat_, &listener, &self);
        }
    }

    static void GlobalRemoved(void *, wl_registry *, std::uint32_t)
    {
    }

    static void Ping(void *, xdg_wm_base *shell, std::uint32_t serial)
    {
        xdg_wm_base_pong(shell, serial);
    }

    static void SeatName(void *, wl_seat *, const char *)
    {
    }

    static void SeatCapabilities(void *data, wl_seat *seat, std::uint32_t capabilities)
    {
        auto &self = *static_cast<Client *>(data);
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !self.pointer_) {
            self.pointer_ = wl_seat_get_pointer(seat);
            static const wl_pointer_listener listener{.enter = PointerEnter,
                                                      .leave = PointerLeave,
                                                      .motion = PointerMotion,
                                                      .button = PointerButton,
                                                      .axis = PointerAxis,
                                                      .frame = PointerFrame,
                                                      .axis_source = PointerAxisSource,
                                                      .axis_stop = PointerAxisStop,
                                                      .axis_discrete = PointerAxisDiscrete};
            wl_pointer_add_listener(self.pointer_, &listener, &self);
        }
        if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !self.keyboard_) {
            self.keyboard_ = wl_seat_get_keyboard(seat);
            static const wl_keyboard_listener listener{.keymap = KeyboardKeymap,
                                                       .enter = KeyboardEnter,
                                                       .leave = KeyboardLeave,
                                                       .key = KeyboardKey,
                                                       .modifiers = KeyboardModifiers,
                                                       .repeat_info = KeyboardRepeat};
            wl_keyboard_add_listener(self.keyboard_, &listener, &self);
        }
    }

    int SurfaceIndex(wl_surface *native) const
    {
        for (const auto &surface : surfaces_) {
            if (surface.native == native) {
                return int(surface.index);
            }
        }
        return -1;
    }

    static void PointerEnter(void *data, wl_pointer *, std::uint32_t, wl_surface *surface,
                             wl_fixed_t x, wl_fixed_t y)
    {
        auto &self = *static_cast<Client *>(data);
        self.pointer_surface_ = self.SurfaceIndex(surface);
        self.observations_.pointer_x = x;
        self.observations_.pointer_y = y;
    }

    static void PointerLeave(void *data, wl_pointer *, std::uint32_t, wl_surface *)
    {
        static_cast<Client *>(data)->pointer_surface_ = -1;
    }

    static void PointerMotion(void *data, wl_pointer *, std::uint32_t, wl_fixed_t x, wl_fixed_t y)
    {
        auto &self = *static_cast<Client *>(data);
        self.observations_.pointer_x = x;
        self.observations_.pointer_y = y;
    }

    static void PointerButton(void *data, wl_pointer *, std::uint32_t, std::uint32_t, std::uint32_t,
                              std::uint32_t)
    {
        auto &self = *static_cast<Client *>(data);
        if (self.pointer_surface_ >= 0) {
            ++self.observations_.buttons[std::size_t(self.pointer_surface_)];
        }
    }

    static void PointerAxis(void *, wl_pointer *, std::uint32_t, std::uint32_t, wl_fixed_t)
    {
    }

    static void PointerFrame(void *, wl_pointer *)
    {
    }

    static void PointerAxisSource(void *, wl_pointer *, std::uint32_t)
    {
    }

    static void PointerAxisStop(void *, wl_pointer *, std::uint32_t, std::uint32_t)
    {
    }

    static void PointerAxisDiscrete(void *, wl_pointer *, std::uint32_t, std::int32_t)
    {
    }

    static void KeyboardKeymap(void *, wl_keyboard *, std::uint32_t, int fd, std::uint32_t)
    {
        close(fd);
    }

    static void KeyboardEnter(void *data, wl_keyboard *, std::uint32_t, wl_surface *surface,
                              wl_array *)
    {
        auto &self = *static_cast<Client *>(data);
        self.observations_.focused = self.SurfaceIndex(surface);
    }

    static void KeyboardLeave(void *data, wl_keyboard *, std::uint32_t, wl_surface *)
    {
        static_cast<Client *>(data)->observations_.focused = -1;
    }

    static void KeyboardKey(void *data, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t,
                            std::uint32_t)
    {
        auto &self = *static_cast<Client *>(data);
        const int index = self.observations_.focused;
        if (index >= 0) {
            ++self.observations_.keys[std::size_t(index)];
        }
    }

    static void KeyboardModifiers(void *, wl_keyboard *, std::uint32_t, std::uint32_t,
                                  std::uint32_t, std::uint32_t, std::uint32_t)
    {
    }

    static void KeyboardRepeat(void *, wl_keyboard *, std::int32_t, std::int32_t)
    {
    }

    static void ToplevelConfigure(void *data, xdg_toplevel *, std::int32_t width,
                                  std::int32_t height, wl_array *)
    {
        auto &surface = *static_cast<Surface *>(data);
        if (width > 0 && height > 0) {
            surface.width = width;
            surface.height = height;
        }
    }

    static void ToplevelClose(void *, xdg_toplevel *)
    {
    }

    static void PopupConfigure(void *data, xdg_popup *, std::int32_t x, std::int32_t y,
                               std::int32_t width, std::int32_t height)
    {
        auto &surface = *static_cast<Surface *>(data);
        surface.x = x;
        surface.y = y;
        surface.width = width;
        surface.height = height;
    }

    static void PopupDone(void *data, xdg_popup *)
    {
        auto &surface = *static_cast<Surface *>(data);
        ++surface.owner->observations_.done[surface.index];
        surface.owner->DestroySurface(surface);
    }

    static void PopupRepositioned(void *data, xdg_popup *, std::uint32_t token)
    {
        auto &surface = *static_cast<Surface *>(data);
        if (token == 17) {
            ++surface.owner->observations_.repositioned;
        }
    }

    static void SurfaceConfigure(void *data, xdg_surface *native, std::uint32_t serial)
    {
        auto &surface = *static_cast<Surface *>(data);
        xdg_surface_ack_configure(native, serial);
        if (surface.index || surface.owner->parent_buffers_) {
            surface.owner->Attach(surface);
        }
        auto &seen = surface.owner->observations_;
        seen.x[surface.index] = surface.x;
        seen.y[surface.index] = surface.y;
        seen.width[surface.index] = surface.width;
        seen.height[surface.index] = surface.height;
        ++seen.configured[surface.index];
    }

    void Attach(Surface &surface)
    {
        const int width = surface.width + surface.inset_x * 2;
        const int height = surface.height + surface.inset_y * 2;
        auto buffer = std::make_unique<Buffer>();
        buffer->size = std::size_t(width) * height * 4;
        const int fd = memfd_create("prism-popup-test", MFD_CLOEXEC);
        Require(fd >= 0, "Cannot create test SHM fd");
        if (ftruncate(fd, off_t(buffer->size)) < 0) {
            close(fd);
            throw std::runtime_error("Cannot size test SHM fd");
        }
        buffer->pixels = mmap(nullptr, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        auto *pool = wl_shm_create_pool(shm_, fd, int(buffer->size));
        close(fd);
        Require(pool && buffer->pixels != MAP_FAILED, "Cannot create test SHM storage");
        buffer->native =
            wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        Require(buffer->native, "Cannot create test buffer");
        std::fill_n(static_cast<std::uint32_t *>(buffer->pixels), std::size_t(width) * height,
                    0xff336699u + surface.index * 0x00330000u);
        if (surface.inset_x || surface.inset_y) {
            xdg_surface_set_window_geometry(surface.xdg, surface.inset_x, surface.inset_y,
                                            surface.width, surface.height);
        }
        wl_surface_attach(surface.native, buffer->native, 0, 0);
        wl_surface_damage_buffer(surface.native, 0, 0, width, height);
        wl_surface_commit(surface.native);
        buffers_.push_back(std::move(buffer));
    }

    void Initialize()
    {
        display_ = wl_display_connect(socket_.c_str());
        Require(display_, "Cannot connect popup test client");
        registry_ = wl_display_get_registry(display_);
        static const wl_registry_listener listener{Global, GlobalRemoved};
        wl_registry_add_listener(registry_, &listener, this);
        Require(wl_display_roundtrip(display_) >= 0 && wl_display_roundtrip(display_) >= 0,
                "Cannot receive popup test globals");
        Require(compositor_ && shell_ && shm_ && seat_, "Missing popup test globals");

        auto &parent = surfaces_[0];
        parent.native = wl_compositor_create_surface(compositor_);
        parent.xdg = xdg_wm_base_get_xdg_surface(shell_, parent.native);
        parent.top = xdg_surface_get_toplevel(parent.xdg);
        parent.inset_x = explicit_ ? 11 : 0;
        parent.inset_y = explicit_ ? 13 : 0;
        static const xdg_surface_listener surface_listener{SurfaceConfigure};
        static const xdg_toplevel_listener top_listener{.configure = ToplevelConfigure,
                                                        .close = ToplevelClose};
        xdg_surface_add_listener(parent.xdg, &surface_listener, &parent);
        xdg_toplevel_add_listener(parent.top, &top_listener, &parent);
        xdg_toplevel_set_app_id(parent.top, "prism.popup-wm-test");
        xdg_toplevel_set_title(parent.top, "Standard popup transport test");
        observations_.ids[0] = wl_proxy_get_id(reinterpret_cast<wl_proxy *>(parent.native));
        wl_surface_commit(parent.native);
    }

    xdg_positioner *Positioner(unsigned index, bool edge = false)
    {
        auto *positioner = xdg_wm_base_create_positioner(shell_);
        xdg_positioner_set_size(positioner, index == 1 ? 120 : 80, index == 1 ? 80 : 48);
        int x = index == 1 ? 80 : 20;
        int y = index == 1 ? 40 : 25;
        if (edge) {
            x = surfaces_[0].width - 24;
            y = surfaces_[0].height - 20;
        }
        xdg_positioner_set_anchor_rect(positioner, x, y, index == 1 ? 24 : 15,
                                       index == 1 ? 20 : 10);
        xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_offset(positioner, 0, index == 1 ? 8 : 4);
        xdg_positioner_set_constraint_adjustment(positioner,
                                                 XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                                                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
        return positioner;
    }

    void CreatePopup(unsigned index)
    {
        auto &surface = surfaces_[index];
        Require(!surface.native && surfaces_[index - 1].native, "Invalid popup test lifecycle");
        surface.native = wl_compositor_create_surface(compositor_);
        surface.xdg = xdg_wm_base_get_xdg_surface(shell_, surface.native);
        auto *positioner = Positioner(index);
        surface.popup = xdg_surface_get_popup(surface.xdg, surfaces_[index - 1].xdg, positioner);
        xdg_positioner_destroy(positioner);
        surface.inset_x = explicit_ ? int(index == 1 ? 6 : 3) : 0;
        surface.inset_y = explicit_ ? int(index == 1 ? 8 : 5) : 0;
        static const xdg_surface_listener surface_listener{SurfaceConfigure};
        static const xdg_popup_listener popup_listener{PopupConfigure, PopupDone,
                                                       PopupRepositioned};
        xdg_surface_add_listener(surface.xdg, &surface_listener, &surface);
        xdg_popup_add_listener(surface.popup, &popup_listener, &surface);
        observations_.ids[index] = wl_proxy_get_id(reinterpret_cast<wl_proxy *>(surface.native));
        wl_surface_commit(surface.native);
    }

    void DestroySurface(Surface &surface)
    {
        if (surface.popup) {
            xdg_popup_destroy(surface.popup);
            surface.popup = nullptr;
        }
        if (surface.top) {
            xdg_toplevel_destroy(surface.top);
            surface.top = nullptr;
        }
        if (surface.xdg) {
            xdg_surface_destroy(surface.xdg);
            surface.xdg = nullptr;
        }
        if (surface.native) {
            wl_surface_destroy(surface.native);
            surface.native = nullptr;
        }
    }

    void ProcessCommand(Command command)
    {
        switch (command) {
        case Command::Popup:
            CreatePopup(1);
            break;
        case Command::Nested:
            CreatePopup(2);
            break;
        case Command::Reposition: {
            auto *positioner = Positioner(1, true);
            xdg_popup_reposition(surfaces_[1].popup, positioner, 17);
            xdg_positioner_destroy(positioner);
            break;
        }
        case Command::Destroy:
            DestroySurface(surfaces_[2]);
            DestroySurface(surfaces_[1]);
            break;
        case Command::Fullscreen:
            xdg_toplevel_set_fullscreen(surfaces_[0].top, nullptr);
            break;
        case Command::Restore:
            xdg_toplevel_unset_fullscreen(surfaces_[0].top);
            break;
        case Command::Unmap:
            parent_buffers_ = false;
            wl_surface_attach(surfaces_[0].native, nullptr, 0, 0);
            wl_surface_commit(surfaces_[0].native);
            break;
        default:
            break;
        }
    }

    bool Pump()
    {
        while (wl_display_prepare_read(display_) != 0) {
            if (wl_display_dispatch_pending(display_) < 0) {
                return false;
            }
        }
        const int flushed = wl_display_flush(display_);
        pollfd source{wl_display_get_fd(display_), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
        const int ready = poll(&source, 1, 5);
        if (ready > 0 && (source.revents & POLLIN)) {
            if (wl_display_read_events(display_) < 0) {
                return false;
            }
        } else {
            wl_display_cancel_read(display_);
        }
        return ready >= 0 && !(source.revents & (POLLERR | POLLHUP | POLLNVAL)) &&
               wl_display_dispatch_pending(display_) >= 0;
    }

    void Close() noexcept
    {
        for (auto it = surfaces_.rbegin(); it != surfaces_.rend(); ++it) {
            DestroySurface(*it);
        }
        buffers_.clear();
        if (pointer_) {
            wl_pointer_release(pointer_);
        }
        if (keyboard_) {
            wl_keyboard_release(keyboard_);
        }
        if (seat_) {
            wl_seat_release(seat_);
        }
        if (shell_) {
            xdg_wm_base_destroy(shell_);
        }
        if (shm_) {
            wl_shm_destroy(shm_);
        }
        if (compositor_) {
            wl_compositor_destroy(compositor_);
        }
        if (registry_) {
            wl_registry_destroy(registry_);
        }
        if (display_) {
            wl_display_flush(display_);
            wl_display_disconnect(display_);
        }
    }

    std::string socket_;
    Observations &observations_;
    bool explicit_{}, parent_buffers_{true};
    wl_display *display_{};
    wl_registry *registry_{};
    wl_compositor *compositor_{};
    wl_shm *shm_{};
    xdg_wm_base *shell_{};
    wl_seat *seat_{};
    wl_pointer *pointer_{};
    wl_keyboard *keyboard_{};
    int pointer_surface_{-1};
    std::array<Surface, 3> surfaces_{};
    std::vector<std::unique_ptr<Buffer>> buffers_;
};

wlr_scene_surface *FindSurface(wlr_scene_node *node, unsigned id)
{
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        auto *surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        return surface && wl_resource_get_id(surface->surface->resource) == id ? surface : nullptr;
    }
    if (node->type != WLR_SCENE_NODE_TREE) {
        return nullptr;
    }
    wlr_scene_node *child;
    wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link)
    {
        if (auto *found = FindSurface(child, id)) {
            return found;
        }
    }
    return nullptr;
}

class Fixture {
public:
    explicit Fixture(bool explicit_geometry)
        : compositor(std::make_shared<wm::Compositor>()), server(compositor),
          client("wayland-prism-popup-wm-" + std::to_string(getpid()), seen, explicit_geometry)
    {
        Require(compositor->Initialize(), "Cannot initialize test compositor");
        Require(server.Initialize("wayland-prism-popup-wm-" + std::to_string(getpid())),
                "Cannot initialize isolated popup WM");
        server.Start();
        Require(server.InstallTheme(test::WmThemeFixture()).success, "Cannot install test theme");
        static const wlr_keyboard_impl keyboard_impl{.name = "prism-popup-test-keyboard"};
        wlr_keyboard_init(&keyboard, &keyboard_impl, "prism-popup-test-keyboard");
        server.HandleNewInput(&keyboard.base);
        thread = std::thread(&Client::Run, &client);
    }

    ~Fixture()
    {
        seen.command = Command::Stop;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!seen.stopped && std::chrono::steady_clock::now() < deadline) {
            server.RunEventLoopIteration(5);
        }
        if (thread.joinable()) {
            thread.join();
        }
        server.Stop();
        wlr_keyboard_finish(&keyboard);
    }

    template <typename Predicate> void Wait(Predicate predicate, const char *message)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!predicate() && !seen.failed && std::chrono::steady_clock::now() < deadline) {
            server.RunEventLoopIteration(5);
        }
        Require(!seen.failed && predicate(), message);
    }

    wlr_scene_surface *Surface(unsigned index)
    {
        const auto id = seen.ids[index].load();
        return id ? FindSurface(&server.GetScene()->tree.node, id) : nullptr;
    }

    bool Mapped(unsigned index)
    {
        auto *surface = Surface(index);
        int x{}, y{};
        return surface && surface->surface->mapped &&
               wlr_scene_node_coords(&surface->buffer->node, &x, &y);
    }

    contracts::LogicalRect ParentBounds()
    {
        const auto snapshot = server.GetLayoutSnapshot();
        Require(snapshot && std::count_if(snapshot->nodes.begin(), snapshot->nodes.end(),
                                          [](const auto &node) {
                                              return node.kind == contracts::LayoutNodeKind::View;
                                          }) == 1,
                "Popup incorrectly became an independent BSP window");
        const auto view =
            std::find_if(snapshot->nodes.begin(), snapshot->nodes.end(), [](const auto &node) {
                return node.kind == contracts::LayoutNodeKind::View;
            });
        return view->target_bounds;
    }

    void CreateChain()
    {
        Wait([this] { return Mapped(0) && Settled(); }, "Owner configure is not committed");
        const auto root_count = seen.configured[1].load();
        seen.command = Command::Popup;
        Wait([this, root_count] { return seen.configured[1] > root_count && Mapped(1); },
             "Initial standard popup did not configure/map");
        const auto nested_count = seen.configured[2].load();
        seen.command = Command::Nested;
        Wait([this, nested_count] { return seen.configured[2] > nested_count && Mapped(2); },
             "Nested standard popup did not configure/map");
        ParentBounds();
    }

    void Click(unsigned index, double x, double y)
    {
        const auto before = seen.buttons[index].load();
        const auto bounds = ParentBounds();
        server.HandleCursorMotion(200, x - server.GetCursor()->x, y - server.GetCursor()->y);
        Require(server.GetSeat()->pointer_state.focused_surface == Surface(index)->surface,
                "Scene hit test routed Popup input to the wrong surface");
        server.HandleCursorButton(201, 272, WLR_BUTTON_PRESSED);
        server.HandleCursorButton(202, 272, WLR_BUTTON_RELEASED);
        Wait([this, index, before] { return seen.buttons[index] >= before + 2; },
             "Real Wayland popup pointer input was not delivered");
        Require(ParentBounds() == bounds, "Popup click changed the BSP owner geometry");
    }

    void Workspace(unsigned number)
    {
        const auto index = xkb_keymap_mod_get_index(keyboard.keymap, XKB_MOD_NAME_LOGO);
        wlr_keyboard_notify_modifiers(&keyboard, 1u << index, 0, 0, 0);
        wlr_keyboard_key_event down{.time_msec = 300,
                                    .keycode = number + 1,
                                    .update_state = true,
                                    .state = WL_KEYBOARD_KEY_STATE_PRESSED};
        wlr_keyboard_notify_key(&keyboard, &down);
        down.state = WL_KEYBOARD_KEY_STATE_RELEASED;
        wlr_keyboard_notify_key(&keyboard, &down);
        wlr_keyboard_notify_modifiers(&keyboard, 0, 0, 0, 0);
    }

    void WaitClosed(unsigned first, unsigned second)
    {
        Wait(
            [this, first, second] {
                return seen.done[1] > first && seen.done[2] > second && !Surface(1) && !Surface(2);
            },
            "Owner transition did not close and remove its entire popup chain");
    }

    bool Settled()
    {
        const auto snapshot = server.GetLayoutSnapshot();
        return std::any_of(snapshot->nodes.begin(), snapshot->nodes.end(), [](const auto &node) {
            return node.kind == contracts::LayoutNodeKind::View && node.visible &&
                   node.has_committed && node.committed_bounds == node.target_bounds;
        });
    }

    wlr_output *Output()
    {
        auto *layout = server.GetOutputLayout();
        Require(!wl_list_empty(&layout->outputs), "Test output disappeared");
        auto *entry = reinterpret_cast<wlr_output_layout_output *>(
            reinterpret_cast<char *>(layout->outputs.next) -
            offsetof(wlr_output_layout_output, link));
        return entry->output;
    }

    void Scale(float scale)
    {
        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_scale(&state, scale);
        const bool accepted = wlr_output_commit_state(Output(), &state);
        wlr_output_state_finish(&state);
        Require(accepted, "Headless output rejected scale commit");
    }

    void Enable(bool enabled)
    {
        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, enabled);
        const bool accepted = wlr_output_commit_state(Output(), &state);
        wlr_output_state_finish(&state);
        Require(accepted, "Headless output rejected enabled commit");
    }

    std::shared_ptr<wm::Compositor> compositor;
    wm::WlrServer server;
    Observations seen;
    Client client;
    wlr_keyboard keyboard{};
    std::thread thread;
};

void RunGeometryAndLifecycle(bool explicit_geometry)
{
    Fixture f(explicit_geometry);
    f.Wait([&f] { return f.seen.ready && f.Mapped(0); }, "Toplevel did not map");
    const auto parent = f.ParentBounds();
    f.CreateChain();
    Require(f.seen.x[1] == 80 && f.seen.y[1] == 68 && f.seen.x[2] == 20 && f.seen.y[2] == 39,
            "Popup parent-local coordinates differ from the positioner");
    f.Click(2, parent.x + 80 + 20 + 10, parent.y + 68 + 39 + 10);
    const int inset_x = explicit_geometry ? 3 : 0;
    const int inset_y = explicit_geometry ? 5 : 0;
    Require(f.seen.pointer_x == wl_fixed_from_int(10 + inset_x) &&
                f.seen.pointer_y == wl_fixed_from_int(10 + inset_y),
            "Nested popup input ignored the configured window-geometry origin");

    const auto before = f.seen.configured[1].load();
    f.seen.command = Command::Reposition;
    f.Wait([&f, before] { return f.seen.repositioned == 1 && f.seen.configured[1] > before; },
           "xdg_popup.reposition did not configure/ack");
    const double popup_x = parent.x + f.seen.x[1].load();
    const double popup_y = parent.y + f.seen.y[1].load();
    Require(popup_x >= 0 && popup_y >= 0 && popup_x + 120 <= 1280 && popup_y + 80 <= 720,
            "Popup unconstrain used the wrong coordinate space");
    f.Click(2, popup_x + 20 + 10, popup_y + 39 + 10);

    f.seen.command = Command::Destroy;
    f.Wait([&f] { return !f.Surface(1) && !f.Surface(2); },
           "Client popup destruction leaked scene nodes");
    f.CreateChain();
    auto first = f.seen.done[1].load();
    auto second = f.seen.done[2].load();
    f.seen.command = Command::Fullscreen;
    f.WaitClosed(first, second);
    f.Wait([&f] { return f.ParentBounds().width == 1280; }, "Fullscreen did not apply");
    f.seen.command = Command::Restore;
    f.Wait([&f, parent] { return f.ParentBounds() == parent; }, "Fullscreen restore did not apply");

    f.CreateChain();
    first = f.seen.done[1].load();
    second = f.seen.done[2].load();
    f.Workspace(2);
    f.WaitClosed(first, second);
    Require(!f.Mapped(0), "Inactive workspace owner still accepts scene input");
    const auto configured_before_rejection = f.seen.configured[1].load();
    const auto dismissed_before_rejection = f.seen.done[1].load();
    f.seen.command = Command::Popup;
    f.Wait(
        [&f, dismissed_before_rejection] {
            return f.seen.done[1] > dismissed_before_rejection && !f.Surface(1);
        },
        "New popup for a hidden mapped owner did not receive deferred popup_done");
    Require(f.seen.configured[1] == configured_before_rejection && !f.seen.failed,
            "Rejected new popup configured/mapped or disconnected its parent");
    f.Workspace(1);
    f.Wait([&f] { return f.Mapped(0); }, "Workspace did not restore owner");
    f.CreateChain();
    first = f.seen.done[1].load();
    second = f.seen.done[2].load();
    const auto before_scale = f.seen.configured[0].load();
    f.Scale(2.0f);
    f.WaitClosed(first, second);
    f.Wait([&f, before_scale] { return f.seen.configured[0] > before_scale && f.Settled(); },
           "Output scale did not reconfigure the owner");
    f.Scale(1.0f);
    f.Wait([&f, parent] { return f.ParentBounds() == parent && f.Settled(); },
           "Output scale restore did not settle");
    f.CreateChain();
    first = f.seen.done[1].load();
    second = f.seen.done[2].load();
    f.Enable(false);
    f.WaitClosed(first, second);
    Require(f.Surface(0) && f.Surface(0)->surface->mapped,
            "Output disable unexpectedly unmapped the parent protocol surface");
    const auto no_output_configures = f.seen.configured[1].load();
    const auto no_output_done = f.seen.done[1].load();
    f.seen.command = Command::Popup;
    f.Wait([&f, no_output_done] { return f.seen.done[1] > no_output_done && !f.Surface(1); },
           "Popup configured against a fabricated output after all outputs were disabled");
    Require(f.seen.configured[1] == no_output_configures && !f.seen.failed,
            "No-output popup rejection configured or disconnected its parent");
    f.Enable(true);
    f.CreateChain();
    first = f.seen.done[1].load();
    second = f.seen.done[2].load();
    f.seen.command = Command::Unmap;
    f.WaitClosed(first, second);
    Require(!f.seen.failed, "Popup lifecycle caused a parent client disconnect");
}
} // namespace

int main()
{
    char runtime_template[] = "/tmp/prism-popup-wm.XXXXXX";
    char *runtime = mkdtemp(runtime_template);
    if (!runtime) {
        return 1;
    }
    setenv("XDG_RUNTIME_DIR", runtime, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    try {
        RunGeometryAndLifecycle(false);
        RunGeometryAndLifecycle(true);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "popup WM gate: %s\n", error.what());
        std::filesystem::remove_all(runtime);
        return 1;
    }
    std::filesystem::remove_all(runtime);
    std::puts("Standard WM popup geometry, nested input and owner lifecycle passed");
    return 0;
}
