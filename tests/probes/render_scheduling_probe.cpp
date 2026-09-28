// Isolated production-WM scheduling regression. Never install this executable.
// Usage: render_scheduling_probe [--gles] [--report /absolute/report.json]
// GLES uses the real renderer selected by WLR_RENDER_DRM_DEVICE; output is
// headless. Pointer injection exercises wlroots input signals, not real-device
// latency. Raw child clients use no SDK timers, DSL or Skia.
#include "../fixtures/wm_theme_fixture.hpp"
#include "prism-surface-effects-client.h"
#include "prism/ipc/ipc_server.hpp"
#include "prism/launch/control_protocol.hpp"
#include "prism/launch/stream.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/wlr_server.hpp"
#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>
extern "C" {
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
}
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
void Require(bool condition, const std::string &reason)
{
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

void Write(int fd, const std::string &text)
{
    std::size_t offset{};
    while (offset < text.size()) {
        const auto size = send(fd, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
        if (size < 0 && errno == EINTR) {
            continue;
        }
        Require(size > 0, "probe channel write failed");
        offset += size;
    }
}

std::vector<Json> ReadMessages(int fd, std::string &pending)
{
    char bytes[4096];
    for (;;) {
        const auto size = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (size > 0) {
            pending.append(bytes, size);
            Require(pending.size() < 65536, "probe channel exceeds bound");
        } else if (size < 0 && errno == EINTR) {
            continue;
        } else if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (!size) {
            break;
        } else {
            throw std::runtime_error("probe channel read failed");
        }
    }
    std::vector<Json> messages;
    for (auto end = pending.find('\n'); end != pending.npos; end = pending.find('\n')) {
        messages.push_back(Json::parse(pending.substr(0, end)));
        pending.erase(0, end + 1);
    }
    return messages;
}

// The client intentionally exposes commit forms that high-level frontends
// normally hide: frame-callback-only and effect-only, with no buffer attach or
// pixel damage. Each draw allocates fresh SHM storage, never modifying a busy
// buffer. Allocation helpers and all fixtures remain in this test executable.
class RawClient {
    struct Buffer {
        wl_buffer *object{};
        void *pixels{MAP_FAILED};
        std::size_t bytes{};

        ~Buffer()
        {
            if (object) {
                wl_buffer_destroy(object);
            }
            if (pixels != MAP_FAILED) {
                munmap(pixels, bytes);
            }
        }
    };

    struct Callback {
        RawClient *owner{};
        int id{};
    };

    int channel_;
    std::string pending_;
    bool desktop_, dock_, running_{true}, ready_{}, backdrop_{};
    int width_{320}, height_{200}, buffer_width_{}, buffer_height_{}, logical_buffer_width_{},
        logical_buffer_height_{}, draws_{};
    wl_output_transform transform_{WL_OUTPUT_TRANSFORM_NORMAL};
    wl_display *display_{};
    wl_registry *registry_{};
    wl_compositor *compositor_{};
    wl_shm *shm_{};
    xdg_wm_base *shell_{};
    wl_surface *surface_{};
    xdg_surface *xdg_{};
    xdg_toplevel *top_{};
    prism_surface_effect_manager_v1 *manager_{};
    prism_surface_effect_v1 *effect_{};
    wl_subcompositor *subcompositor_{};
    wl_surface *sub_surface_{};
    wl_subsurface *subsurface_{};
    wl_buffer *sub_buffer_{};
    wl_seat *seat_{};
    wl_pointer *pointer_{};
    std::vector<std::unique_ptr<Buffer>> buffers_;
    std::vector<std::uint32_t> canonical_;
    std::map<wl_callback *, std::unique_ptr<Callback>> callbacks_;

    void Emit(Json message)
    {
        Write(channel_, message.dump() + "\n");
    }

    static void Global(void *data, wl_registry *registry, uint32_t name, const char *interface,
                       uint32_t version)
    {
        auto &self = *static_cast<RawClient *>(data);
        if (!std::strcmp(interface, wl_compositor_interface.name)) {
            self.compositor_ = static_cast<wl_compositor *>(
                wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 5u)));
        } else if (!std::strcmp(interface, wl_shm_interface.name)) {
            self.shm_ =
                static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        } else if (!std::strcmp(interface, wl_subcompositor_interface.name)) {
            self.subcompositor_ = static_cast<wl_subcompositor *>(
                wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
        } else if (!std::strcmp(interface, xdg_wm_base_interface.name)) {
            self.shell_ = static_cast<xdg_wm_base *>(
                wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
            static const xdg_wm_base_listener listener{
                .ping = [](void *, xdg_wm_base *shell, uint32_t serial) {
                    xdg_wm_base_pong(shell, serial);
                }};
            xdg_wm_base_add_listener(self.shell_, &listener, &self);
        } else if (!std::strcmp(interface, prism_surface_effect_manager_v1_interface.name)) {
            self.manager_ = static_cast<prism_surface_effect_manager_v1 *>(
                wl_registry_bind(registry, name, &prism_surface_effect_manager_v1_interface, 1));
            static const prism_surface_effect_manager_v1_listener listener{
                .capabilities = [](void *data, prism_surface_effect_manager_v1 *,
                                   uint32_t supported) {
                    static_cast<RawClient *>(data)->backdrop_ = supported != 0;
                }};
            prism_surface_effect_manager_v1_add_listener(self.manager_, &listener, &self);
        } else if (!std::strcmp(interface, wl_seat_interface.name)) {
            self.seat_ = static_cast<wl_seat *>(
                wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
            static const wl_seat_listener listener{
                .capabilities =
                    [](void *data, wl_seat *seat, uint32_t capabilities) {
                        auto &self = *static_cast<RawClient *>(data);
                        if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) || self.pointer_) {
                            return;
                        }
                        self.pointer_ = wl_seat_get_pointer(seat);
                        static const wl_pointer_listener pointer_listener = [] {
                            // Bind at most version 5; newer listener tails remain
                            // zero without depending on the installed header ABI.
                            wl_pointer_listener listener{};
                            listener.enter = [](void *data, wl_pointer *, uint32_t, wl_surface *,
                                                wl_fixed_t, wl_fixed_t) {
                                static_cast<RawClient *>(data)->Emit(
                                    {{"event", "pointer"}, {"kind", "enter"}});
                            };
                            listener.leave = [](void *, wl_pointer *, uint32_t, wl_surface *) {
                            };
                            listener.motion = [](void *data, wl_pointer *, uint32_t, wl_fixed_t,
                                                 wl_fixed_t) {
                                static_cast<RawClient *>(data)->Emit(
                                    {{"event", "pointer"}, {"kind", "motion"}});
                            };
                            listener.button = [](void *, wl_pointer *, uint32_t, uint32_t, uint32_t,
                                                 uint32_t) {
                            };
                            listener.axis = [](void *, wl_pointer *, uint32_t, uint32_t,
                                               wl_fixed_t) {
                            };
                            listener.frame = [](void *, wl_pointer *) {
                            };
                            listener.axis_source = [](void *, wl_pointer *, uint32_t) {
                            };
                            listener.axis_stop = [](void *, wl_pointer *, uint32_t, uint32_t) {
                            };
                            listener.axis_discrete = [](void *, wl_pointer *, uint32_t, int32_t) {
                            };
                            return listener;
                        }();
                        wl_pointer_add_listener(self.pointer_, &pointer_listener, &self);
                    },
                .name =
                    [](void *, wl_seat *, const char *) {
                    }};
            wl_seat_add_listener(self.seat_, &listener, &self);
        }
    }

    void Frame(int id)
    {
        auto data = std::make_unique<Callback>();
        data->owner = this;
        data->id = id;
        auto *callback = wl_surface_frame(surface_);
        static const wl_callback_listener listener{
            .done = [](void *data, wl_callback *callback, uint32_t) {
                auto *value = static_cast<Callback *>(data);
                auto &self = *value->owner;
                const int id = value->id;
                self.Emit({{"event", "done"}, {"id", id}});
                wl_callback_destroy(callback);
                self.callbacks_.erase(callback);
            }};
        wl_callback_add_listener(callback, &listener, data.get());
        callbacks_.emplace(callback, std::move(data));
    }

    wl_buffer *AllocateBuffer(int width, int height, std::uint32_t color,
                              const std::vector<std::uint32_t> *contents = nullptr)
    {
        auto buffer = std::make_unique<Buffer>();
        buffer->bytes = static_cast<std::size_t>(width) * height * 4;
        const int fd = memfd_create("prism-scheduling-fixture", MFD_CLOEXEC);
        Require(fd >= 0, "client memfd_create failed");
        if (ftruncate(fd, buffer->bytes) < 0) {
            close(fd);
            throw std::runtime_error("client ftruncate failed");
        }
        buffer->pixels = mmap(nullptr, buffer->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (buffer->pixels == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("client mmap failed");
        }
        auto *pool = wl_shm_create_pool(shm_, fd, static_cast<int>(buffer->bytes));
        close(fd);
        buffer->object =
            wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        static const wl_buffer_listener buffer_listener{.release = [](void *, wl_buffer *) {
        }};
        wl_buffer_add_listener(buffer->object, &buffer_listener, nullptr);
        if (contents) {
            Require(contents->size() == buffer->bytes / 4, "canonical buffer size mismatch");
            std::copy(contents->begin(), contents->end(),
                      static_cast<std::uint32_t *>(buffer->pixels));
        } else {
            std::fill_n(static_cast<uint32_t *>(buffer->pixels), buffer->bytes / 4, color);
        }
        auto *object = buffer->object;
        buffers_.push_back(std::move(buffer));
        return object;
    }

    struct PixelRect {
        int x, y, width, height;
    };

    PixelRect BufferRect(int x, int y, int width, int height) const
    {
        // Match pinned wlroots 0.18 surface_damage -> buffer_damage: it applies
        // inverse(transform). Its effective buffer -> surface damage conversion
        // applies transform. Keep this fixture math independent of WM helpers.
        if (transform_ == WL_OUTPUT_TRANSFORM_90) {
            return {y, width_ - x - width, height, width};
        }
        if (transform_ == WL_OUTPUT_TRANSFORM_270) {
            return {height_ - y - height, x, height, width};
        }
        return {x, y, width, height};
    }

    void AttachCanonical(int id, bool full_damage)
    {
        const bool rotated = transform_ != WL_OUTPUT_TRANSFORM_NORMAL;
        buffer_width_ = rotated ? height_ : width_;
        buffer_height_ = rotated ? width_ : height_;
        auto *buffer = AllocateBuffer(buffer_width_, buffer_height_, 0, &canonical_);
        Frame(id);
        wl_surface_attach(surface_, buffer, 0, 0);
        if (full_damage) {
            wl_surface_damage_buffer(surface_, 0, 0, buffer_width_, buffer_height_);
        }
        logical_buffer_width_ = width_;
        logical_buffer_height_ = height_;
    }

    void Draw(int id)
    {
        ++draws_;
        const auto color = dock_ ? 0U
                                 : (desktop_ ? (draws_ % 2 ? 0xff38485cU : 0xff5c4838U)
                                             : (draws_ % 2 ? 0x70304050U : 0x70504030U));
        canonical_.assign(static_cast<std::size_t>(width_) * height_, color);
        AttachCanonical(id, true);
        wl_surface_commit(surface_);
    }

    void Sync(int id)
    {
        auto *sync = wl_display_sync(display_);
        auto data = std::make_unique<Callback>();
        data->owner = this;
        data->id = id;
        static const wl_callback_listener listener{
            .done = [](void *data, wl_callback *callback, uint32_t) {
                auto *value = static_cast<Callback *>(data);
                auto &self = *value->owner;
                const int id = value->id;
                self.Emit({{"event", "ack"}, {"id", id}});
                wl_callback_destroy(callback);
                self.callbacks_.erase(callback);
            }};
        wl_callback_add_listener(sync, &listener, data.get());
        callbacks_.emplace(sync, std::move(data));
    }

    void Command(const Json &command)
    {
        const auto action = command.at("action").get<std::string>();
        const int id = command.value("id", 0);
        if (action == "frame") {
            Frame(id);
            wl_surface_commit(surface_);
        } else if (action == "draw") {
            Draw(id);
        } else if (action == "patch") {
            const int x = command.at("x").get<int>(), y = command.at("y").get<int>();
            const int width = command.at("width").get<int>(),
                      height = command.at("height").get<int>();
            const auto color = command.at("color").get<std::uint32_t>();
            Require(x >= 0 && y >= 0 && width > 0 && height > 0 && x + width <= width_ &&
                        y + height <= height_,
                    "invalid damage patch");
            const auto rect = BufferRect(x, y, width, height);
            for (int row = rect.y; row < rect.y + rect.height; ++row) {
                std::fill_n(canonical_.begin() + static_cast<std::size_t>(row) * buffer_width_ +
                                rect.x,
                            rect.width, color);
            }
            // A fresh buffer contains an exact copy of all unaffected pixels.
            // Never mutate a currently busy producer buffer for this fixture.
            AttachCanonical(id, false);
            wl_surface_damage_buffer(surface_, rect.x, rect.y, rect.width, rect.height);
            wl_surface_commit(surface_);
        } else if (action == "rotated_pattern") {
            Require(desktop_, "rotation fixture must use Desktop");
            const int degrees = command.at("degrees").get<int>();
            Require(degrees == 0 || degrees == 90 || degrees == 270,
                    "unsupported fixture rotation");
            transform_ = degrees == 90 ? WL_OUTPUT_TRANSFORM_90
                                       : (degrees == 270 ? WL_OUTPUT_TRANSFORM_270
                                                         : WL_OUTPUT_TRANSFORM_NORMAL);
            canonical_.assign(static_cast<std::size_t>(width_) * height_, 0xff305030U);
            const int stride = transform_ == WL_OUTPUT_TRANSFORM_NORMAL ? width_ : height_;
            for (int y = 0; y < height_; ++y) {
                for (int x = 0; x < width_; ++x) {
                    const auto rect = BufferRect(x, y, 1, 1);
                    std::uint32_t color = 0xff305030U;
                    if (x >= 40 && x < 136 && y >= 80 && y < 152) {
                        color = 0xffe61e28U;
                    }
                    if (x >= 504 && x < 600 && y >= 268 && y < 340) {
                        color = 0xff1932ebU;
                    }
                    if (x >= 470 && x < 540 && y >= 330 && y < 385) {
                        color = 0xffe1c319U;
                    }
                    canonical_[static_cast<std::size_t>(rect.y) * stride + rect.x] = color;
                }
            }
            wl_surface_set_buffer_transform(surface_, transform_);
            AttachCanonical(id, true);
            wl_surface_commit(surface_);
            const auto yellow = BufferRect(480, 355, 1, 1);
            Emit({{"event", "pattern"},
                  {"id", id},
                  {"degrees", degrees},
                  {"logical_width", width_},
                  {"logical_height", height_},
                  {"buffer_width", buffer_width_},
                  {"buffer_height", buffer_height_},
                  {"canonical_yellow",
                   canonical_[static_cast<std::size_t>(yellow.y) * buffer_width_ + yellow.x]}});
        } else if (action == "offset") {
            Require(desktop_ && wl_proxy_get_version(reinterpret_cast<wl_proxy *>(surface_)) >= 5,
                    "offset fixture needs wl_surface v5");
            wl_surface_offset(surface_, 1, 0);
            wl_surface_commit(surface_);
            Sync(id);
        } else if (action == "metadata") {
            const auto kind = command.at("kind").get<std::string>();
            auto *region = wl_compositor_create_region(compositor_);
            if (kind == "input") {
                wl_region_add(region, 0, 0, std::max(1, width_ - 16), std::max(1, height_ - 16));
                wl_surface_set_input_region(surface_, region);
            } else if (kind == "opaque") {
                Require(desktop_, "opaque fixture must declare actually opaque pixels");
                wl_region_add(region, 0, 0, width_, height_);
                wl_surface_set_opaque_region(surface_, region);
            } else {
                wl_region_destroy(region);
                throw std::runtime_error("invalid metadata fixture");
            }
            wl_region_destroy(region);
            wl_surface_commit(surface_);
            Sync(id);
        } else if (action == "effects") {
            Require(effect_ && backdrop_, "GLES effect capability absent");
            const int count = command.at("count");
            const double blur = command.at("blur");
            Require(count >= 0 && count <= 2, "fixture supports zero, one or two effect regions");
            prism_surface_effect_v1_clear(effect_);
            for (int i = 0; i < count; ++i) {
                prism_surface_effect_v1_add_region(
                    effect_, wl_fixed_from_int(command.value("x", 12) + i * 144),
                    wl_fixed_from_int(command.value("y", 12)),
                    wl_fixed_from_int(command.value("width", 100)),
                    wl_fixed_from_int(command.value("height", 64)), wl_fixed_from_int(8),
                    wl_fixed_from_double(blur));
            }
            // Deliberately no attach, damage or frame request in this commit.
            wl_surface_commit(surface_);
            Sync(id);
        } else if (action == "sub_create") {
            Require(desktop_ && subcompositor_ && !subsurface_,
                    "invalid subsurface fixture creation");
            sub_surface_ = wl_compositor_create_surface(compositor_);
            subsurface_ = wl_subcompositor_get_subsurface(subcompositor_, sub_surface_, surface_);
            sub_buffer_ = AllocateBuffer(64, 64, 0xffd84e62);
            wl_subsurface_set_position(subsurface_, command.value("x", 60),
                                       command.value("y", 100));
            wl_surface_attach(sub_surface_, sub_buffer_, 0, 0);
            wl_surface_damage_buffer(sub_surface_, 0, 0, 64, 64);
            wl_surface_commit(sub_surface_);
            wl_surface_commit(surface_);
            Sync(id);
        } else if (action == "sub_move" || action == "sub_order" || action == "sub_unmap" ||
                   action == "sub_map" || action == "sub_patch") {
            Require(subsurface_, "subsurface fixture absent");
            if (action == "sub_move") {
                wl_subsurface_set_position(subsurface_, command.value("x", 80),
                                           command.value("y", 100));
            } else if (action == "sub_order") {
                if (command.at("above").get<bool>()) {
                    wl_subsurface_place_above(subsurface_, surface_);
                } else {
                    wl_subsurface_place_below(subsurface_, surface_);
                }
            } else {
                if (action == "sub_patch") {
                    sub_buffer_ = AllocateBuffer(64, 64, command.at("color").get<std::uint32_t>());
                    wl_surface_damage_buffer(sub_surface_, 0, 0, 64, 64);
                }
                wl_surface_attach(sub_surface_, action == "sub_unmap" ? nullptr : sub_buffer_, 0,
                                  0);
                wl_surface_commit(sub_surface_);
            }
            // Parent has no buffer/damage/frame fields: only synchronized child
            // geometry, ordering or mapping changes become visible here.
            wl_surface_commit(surface_);
            Sync(id);
        } else if (action == "sub_delete") {
            Require(subsurface_, "subsurface fixture absent");
            wl_subsurface_destroy(subsurface_);
            subsurface_ = nullptr;
            wl_surface_destroy(sub_surface_);
            sub_surface_ = nullptr;
            sub_buffer_ = nullptr;
            wl_surface_commit(surface_);
            Sync(id);
        } else if (action == "geometry") {
            Require(!desktop_ && buffer_width_ > 8 && buffer_height_ > 8,
                    "invalid geometry fixture");
            const int origin = command.at("origin").get<int>();
            Require(origin == 0 || origin == 4, "invalid fixture origin");
            // Leave an eight-pixel reserve first; the next commit can change
            // only the origin while retaining the same effective width/height.
            xdg_surface_set_window_geometry(xdg_, origin, origin, buffer_width_ - 8,
                                            buffer_height_ - 8);
            wl_surface_commit(surface_);
            Sync(id);
        } else if (action == "quit") {
            running_ = false;
        } else {
            throw std::runtime_error("unknown fixture command");
        }
    }

public:
    RawClient(int channel, std::string_view role)
        : channel_(channel), desktop_(role == "desktop"), dock_(role == "dock")
    {
    }

    ~RawClient()
    {
        for (auto &[callback, value] : callbacks_) {
            wl_callback_destroy(callback);
        }
        callbacks_.clear();
        if (effect_) {
            prism_surface_effect_v1_destroy(effect_);
        }
        if (top_) {
            xdg_toplevel_destroy(top_);
        }
        if (xdg_) {
            xdg_surface_destroy(xdg_);
        }
        if (subsurface_) {
            wl_subsurface_destroy(subsurface_);
        }
        if (sub_surface_) {
            wl_surface_destroy(sub_surface_);
        }
        if (surface_) {
            wl_surface_destroy(surface_);
        }
        buffers_.clear();
        if (pointer_) {
            wl_pointer_release(pointer_);
        }
        if (seat_) {
            wl_seat_release(seat_);
        }
        if (manager_) {
            prism_surface_effect_manager_v1_destroy(manager_);
        }
        if (shell_) {
            xdg_wm_base_destroy(shell_);
        }
        if (shm_) {
            wl_shm_destroy(shm_);
        }
        if (subcompositor_) {
            wl_subcompositor_destroy(subcompositor_);
        }
        if (compositor_) {
            wl_compositor_destroy(compositor_);
        }
        if (registry_) {
            wl_registry_destroy(registry_);
        }
        if (display_) {
            wl_display_disconnect(display_);
        }
    }

    int Run(const char *socket)
    {
        // The trusted parent grants this PID before permitting a Wayland bind.
        std::string start;
        char byte{};
        while (read(channel_, &byte, 1) == 1 && byte != '\n') {
            start += byte;
        }
        Require(start == "start", "fixture started without trusted registration");
        display_ = wl_display_connect(socket);
        Require(display_, "client connection failed");
        registry_ = wl_display_get_registry(display_);
        static const wl_registry_listener registry_listener{
            .global = Global, .global_remove = [](void *, wl_registry *, uint32_t) {
            }};
        wl_registry_add_listener(registry_, &registry_listener, this);
        Require(wl_display_roundtrip(display_) >= 0 && wl_display_roundtrip(display_) >= 0,
                "client registry roundtrip failed");
        Require(compositor_ && shm_ && shell_, "required Wayland globals absent");
        surface_ = wl_compositor_create_surface(compositor_);
        if (manager_ && !desktop_) {
            effect_ = prism_surface_effect_manager_v1_get_surface_effect(manager_, surface_);
        }
        xdg_ = xdg_wm_base_get_xdg_surface(shell_, surface_);
        top_ = xdg_surface_get_toplevel(xdg_);
        xdg_toplevel_set_app_id(top_, desktop_ ? "prism_desktop"
                                               : (dock_ ? "prism_dock" : "prism.scheduling-app"));
        xdg_toplevel_set_title(top_, desktop_ ? "Scheduling static desktop"
                                              : (dock_ ? "Scheduling transparent Dock"
                                                       : "Scheduling static application"));
        static const xdg_toplevel_listener top_listener = [] {
            xdg_toplevel_listener listener{}; // xdg_wm_base is bound at v1.
            listener.configure = [](void *data, xdg_toplevel *, int32_t width, int32_t height,
                                    wl_array *) {
                auto &self = *static_cast<RawClient *>(data);
                if (width > 0) {
                    self.width_ = width;
                }
                if (height > 0) {
                    self.height_ = height;
                }
            };
            listener.close = [](void *data, xdg_toplevel *) {
                static_cast<RawClient *>(data)->running_ = false;
            };
            return listener;
        }();
        xdg_toplevel_add_listener(top_, &top_listener, this);
        static const xdg_surface_listener surface_listener{
            .configure = [](void *data, xdg_surface *surface, uint32_t serial) {
                auto &self = *static_cast<RawClient *>(data);
                xdg_surface_ack_configure(surface, serial);
                // Attach storage matching actual configure dimensions. Activation
                // acknowledgements alone stay metadata-only instead of producing
                // fixture redraws that could conceal a scheduling self-loop.
                if (!self.ready_) {
                    self.Draw(0);
                    self.ready_ = true;
                    self.Emit({{"event", "ready"},
                               {"width", self.width_},
                               {"height", self.height_},
                               {"backdrop", self.backdrop_}});
                } else if (self.width_ != self.logical_buffer_width_ ||
                           self.height_ != self.logical_buffer_height_) {
                    self.Draw(0);
                } else {
                    wl_surface_commit(self.surface_);
                }
            }};
        xdg_surface_add_listener(xdg_, &surface_listener, this);
        wl_surface_commit(surface_);
        while (running_) {
            while (wl_display_prepare_read(display_) != 0) {
                Require(wl_display_dispatch_pending(display_) >= 0, "client dispatch failed");
            }
            const int flushed = wl_display_flush(display_);
            if (flushed < 0 && errno != EAGAIN) {
                wl_display_cancel_read(display_);
                throw std::runtime_error("client flush failed");
            }
            pollfd fds[] = {{wl_display_get_fd(display_),
                             static_cast<short>(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0},
                            {channel_, POLLIN, 0}};
            const int result = poll(fds, 2, 1000);
            if (result < 0) {
                wl_display_cancel_read(display_);
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("client poll failed");
            }
            if (fds[0].revents & POLLIN) {
                Require(wl_display_read_events(display_) >= 0, "client read failed");
            } else {
                wl_display_cancel_read(display_);
            }
            Require(!(fds[0].revents & (POLLERR | POLLHUP)), "Wayland connection closed");
            Require(wl_display_dispatch_pending(display_) >= 0, "client pending dispatch failed");
            if (fds[1].revents & POLLIN) {
                for (const auto &command : ReadMessages(channel_, pending_)) {
                    Command(command);
                }
            }
            if (fds[1].revents & (POLLERR | POLLHUP)) {
                running_ = false;
            }
        }
        return 0;
    }
};

void Until(prism::wm::WlrServer &server, const std::function<bool()> &condition,
           const std::string &reason, std::chrono::milliseconds timeout = 5000ms)
{
    const auto deadline = Clock::now() + timeout;
    while (!condition() && Clock::now() < deadline) {
        server.RunEventLoopIteration(5);
    }
    Require(condition(), reason);
}

void RunFor(prism::wm::WlrServer &server, std::chrono::milliseconds duration)
{
    const auto end = Clock::now() + duration;
    while (Clock::now() < end) {
        server.RunEventLoopIteration(5);
    }
}

Json Command(prism::wm::WlrServer &server, const std::string &command)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Require(fd >= 0, "IPC socket failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = server.GetIpcServer()->GetSocketPath();
    Require(path.size() < sizeof(address.sun_path), "IPC socket path too long");
    std::copy(path.begin(), path.end(), address.sun_path);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        close(fd);
        throw std::runtime_error("IPC connect failed");
    }
    Write(fd, command + "\n");
    std::string response;
    Json status;
    try {
        Until(
            server,
            [&] {
                char bytes[8192];
                const auto size = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
                if (size > 0) {
                    response.append(bytes, size);
                }
                Require(response.size() < 262144, "IPC response exceeds bound");
                status = Json::parse(response, nullptr, false);
                return !status.is_discarded();
            },
            "IPC command timed out: " + command);
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);
    return status;
}

Json Status(prism::wm::WlrServer &server)
{
    return Command(server, "get_status");
}

std::uint64_t EffectCount(const Json &status, const char *field)
{
    return status.at("performance").at("effects_work").at(field).get<std::uint64_t>();
}

std::uint64_t ScheduleCount(const Json &status, const char *field)
{
    return status.at("performance").at("scheduling").at(field).get<std::uint64_t>();
}

Json ActualCommits(prism::wm::WlrServer &server)
{
    Json result = Json::object();
    wlr_scene_output *output;
    wl_list_for_each(output, &server.GetScene()->outputs, link) result[output->output->name] =
        output->output->commit_seq;
    return result;
}

Json ClientSampling(prism::wm::WlrServer &server, pid_t pid)
{
    Json result = Json::array();
    auto visit = [&](auto &&self, wlr_scene_node *node) -> void {
        if (!node->enabled) {
            return;
        }
        if (node->type == WLR_SCENE_NODE_TREE) {
            wlr_scene_node *child;
            wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link)
                self(self, child);
        } else if (node->type == WLR_SCENE_NODE_BUFFER) {
            auto *buffer = wlr_scene_buffer_from_node(node);
            auto *surface = wlr_scene_surface_try_from_buffer(buffer);
            if (!surface) {
                return;
            }
            pid_t peer{};
            wl_client_get_credentials(wl_resource_get_client(surface->surface->resource), &peer,
                                      nullptr, nullptr);
            if (peer != pid) {
                return;
            }
            int x{}, y{};
            if (!wlr_scene_node_coords(node, &x, &y)) {
                return;
            }
            result.push_back({{"x", x},
                              {"y", y},
                              {"width", buffer->dst_width},
                              {"height", buffer->dst_height},
                              {"source_x", buffer->src_box.x},
                              {"source_y", buffer->src_box.y},
                              {"source_width", buffer->src_box.width},
                              {"source_height", buffer->src_box.height}});
        }
    };
    visit(visit, &server.GetScene()->tree.node);
    Require(!result.empty(), "mapped client sampling fixture absent");
    return result;
}

// Test-only readback of the actual last submitted output, without a screencopy
// request that would create extra frame demand. Holding one output buffer and
// synchronous ROI reads intentionally excludes this probe from performance runs.
class OutputPixels {
    struct Hook {
        wl_listener listener{};
        OutputPixels *owner{};
    };

    wlr_output *output_{};
    wlr_renderer *renderer_{};
    wlr_buffer *buffer_{};
    Hook committed_, destroyed_;

public:
    explicit OutputPixels(prism::wm::WlrServer &server) : renderer_(server.GetRenderer())
    {
        wlr_scene_output *output;
        wl_list_for_each(output, &server.GetScene()->outputs, link)
        {
            output_ = output->output;
            break;
        }
        Require(output_, "no output for readback fixture");
        committed_.owner = this;
        destroyed_.owner = this;
        committed_.listener.notify = [](wl_listener *listener, void *data) {
            auto &self = *reinterpret_cast<Hook *>(listener)->owner;
            const auto *event = static_cast<wlr_output_event_commit *>(data);
            if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER)) {
                return;
            }
            auto *next = event->state->buffer ? wlr_buffer_lock(event->state->buffer) : nullptr;
            if (self.buffer_) {
                wlr_buffer_unlock(self.buffer_);
            }
            self.buffer_ = next;
        };
        destroyed_.listener.notify = [](wl_listener *listener, void *) {
            auto &self = *reinterpret_cast<Hook *>(listener)->owner;
            wl_list_remove(&self.committed_.listener.link);
            wl_list_init(&self.committed_.listener.link);
            wl_list_remove(&self.destroyed_.listener.link);
            wl_list_init(&self.destroyed_.listener.link);
            if (self.buffer_) {
                wlr_buffer_unlock(self.buffer_);
            }
            self.buffer_ = nullptr;
            self.output_ = nullptr;
        };
        wl_signal_add(&output_->events.commit, &committed_.listener);
        wl_signal_add(&output_->events.destroy, &destroyed_.listener);
    }

    ~OutputPixels()
    {
        wl_list_remove(&committed_.listener.link);
        wl_list_remove(&destroyed_.listener.link);
        if (buffer_) {
            wlr_buffer_unlock(buffer_);
        }
    }

    std::vector<std::uint8_t> Read(int x, int y, int width, int height) const
    {
        Require(buffer_ && x >= 0 && y >= 0 && width > 0 && height > 0 &&
                    x + width <= buffer_->width && y + height <= buffer_->height,
                "invalid output ROI");
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
        auto *texture = wlr_texture_from_buffer(renderer_, buffer_);
        Require(texture, "output texture import for ROI failed");
        const wlr_texture_read_pixels_options options{.data = pixels.data(),
                                                      .format = DRM_FORMAT_ABGR8888,
                                                      .stride =
                                                          static_cast<std::uint32_t>(width * 4),
                                                      .dst_x = 0,
                                                      .dst_y = 0,
                                                      .src_box = {x, y, width, height}};
        const bool success = wlr_texture_read_pixels(texture, &options);
        wlr_texture_destroy(texture);
        Require(success, "actual output ROI readback failed");
        return pixels;
    }
};

std::array<double, 3> MeanRgb(const std::vector<std::uint8_t> &pixels)
{
    Require(!pixels.empty() && pixels.size() % 4 == 0, "invalid RGB sample");
    std::array<double, 3> mean{};
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        for (int channel = 0; channel < 3; ++channel) {
            mean[channel] += pixels[i + channel];
        }
    }
    for (auto &value : mean) {
        value /= pixels.size() / 4;
    }
    return mean;
}

double MeanChange(const std::vector<std::uint8_t> &before, const std::vector<std::uint8_t> &after)
{
    const auto a = MeanRgb(before), b = MeanRgb(after);
    double difference{};
    for (int channel = 0; channel < 3; ++channel) {
        difference = std::max(difference, std::abs(a[channel] - b[channel]));
    }
    return difference;
}

class Child {
    prism::wm::WlrServer &server_;
    int fd_{-1};
    pid_t pid_{-1};
    std::string pending_;

public:
    Json ready;
    std::set<int> done, ack;
    std::map<int, Json> patterns;
    unsigned pointer_events{};

    Child(prism::wm::WlrServer &server, const std::string &executable, const std::string &display,
          const char *role)
        : server_(server)
    {
        int channels[2];
        Require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channels) == 0,
                "client socketpair failed");
        const auto number = std::to_string(channels[1]);
        std::array<char *, 6> args{
            const_cast<char *>(executable.c_str()), const_cast<char *>("--client"),
            const_cast<char *>(display.c_str()),    const_cast<char *>(role),
            const_cast<char *>(number.c_str()),     nullptr};
        pid_ = fork();
        if (!pid_) {
            close(channels[0]);
            fcntl(channels[1], F_SETFD, 0);
            execv(args[0], args.data());
            _exit(127);
        }
        close(channels[1]);
        if (pid_ < 0) {
            close(channels[0]);
            throw std::runtime_error("client fork failed");
        }
        fd_ = channels[0];
    }

    ~Child()
    {
        if (pid_ <= 0) {
            return;
        }
        try {
            Send({{"action", "quit"}});
        } catch (...) {
        }
        const auto end = Clock::now() + 1s;
        int status{};
        while (waitpid(pid_, &status, WNOHANG) == 0 && Clock::now() < end) {
            std::this_thread::sleep_for(5ms);
        }
        if (waitpid(pid_, &status, WNOHANG) == 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        }
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    pid_t Pid() const
    {
        return pid_;
    }

    void Start()
    {
        Write(fd_, "start\n");
    }

    void Send(Json command)
    {
        Write(fd_, command.dump() + "\n");
    }

    void Drain()
    {
        for (const auto &message : ReadMessages(fd_, pending_)) {
            const auto event = message.value("event", "");
            if (event == "error") {
                throw std::runtime_error("client error: " + message.dump());
            }
            if (event == "ready") {
                ready = message;
            } else if (event == "done") {
                done.insert(message.at("id").get<int>());
            } else if (event == "ack") {
                ack.insert(message.at("id").get<int>());
            } else if (event == "pattern") {
                patterns[message.at("id").get<int>()] = message;
            } else if (event == "pointer") {
                ++pointer_events;
            }
        }
        int status{};
        const auto dead = waitpid(pid_, &status, WNOHANG);
        Require(dead == 0, "raw client exited before scenario completion");
    }

    void WaitReady()
    {
        Until(
            server_,
            [&] {
                Drain();
                return !ready.is_null() && done.contains(0);
            },
            "initial surface/frame callback timed out");
    }

    void WaitDone(int id)
    {
        Until(
            server_,
            [&] {
                Drain();
                return done.contains(id);
            },
            "callback-only/redraw done timed out id=" + std::to_string(id));
    }

    void WaitAck(int id)
    {
        Until(
            server_,
            [&] {
                Drain();
                return ack.contains(id);
            },
            "effect-only protocol sync timed out");
    }
};

struct Pointer {
    wlr_pointer pointer{};

    explicit Pointer(prism::wm::WlrServer &server)
    {
        static const wlr_pointer_impl implementation{"prism-render-scheduling-fixture"};
        wlr_pointer_init(&pointer, &implementation, "Scheduling test pointer");
        server.HandleNewInput(&pointer.base);
    }

    ~Pointer()
    {
        wlr_pointer_finish(&pointer);
    }

    void Absolute(double x, double y)
    {
        wlr_pointer_motion_absolute_event event{};
        event.pointer = &pointer;
        event.time_msec = static_cast<std::uint32_t>(prism::launch::MonotonicNs() / 1000000);
        event.x = x;
        event.y = y;
        wl_signal_emit_mutable(&pointer.events.motion_absolute, &event);
        wl_signal_emit_mutable(&pointer.events.frame, nullptr);
    }
};

int RunScenarios(const std::string &executable, bool gles, const std::filesystem::path &report_path,
                 int control_fd, int relay_fd)
{
    char directory[] = "/tmp/prism-render-scheduling.XXXXXX";
    Require(mkdtemp(directory), "runtime directory failed");
    setenv("XDG_RUNTIME_DIR", directory, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", gles ? "gles2" : "pixman", 1);
    unsetenv("WAYLAND_SOCKET");
    Json report{{"schema_version", 1},
                {"backend", "headless"},
                {"gles_requested", gles},
                {"scenarios", Json::array()}};
    auto compositor = std::make_shared<prism::wm::Compositor>();
    Require(compositor->Initialize(), "compositor init failed");
    prism::wm::WlrServer server(compositor);
    auto save = [&] {
        if (!report_path.empty()) {
            std::ofstream file(report_path);
            file << report.dump(2) << '\n';
        }
    };
    try {
        const auto display = "wayland-render-scheduling-" + std::to_string(getpid());
        Require(server.Initialize(display), "WM init failed");
        server.Start();
        Require(server.InstallTheme(prism::test::WmThemeFixture()).success,
                "typed fixture theme rejected");
        const auto outputs = server.GetOutputsInfo();
        Require(outputs.size() == 1, "probe requires one isolated output");
        Require(server.SetOutputMode(outputs.front().name, 640, 420, 90000),
                "headless explicit refresh change failed");
        Require(server.GetOutputsInfo().front().refresh_mhz == 90000,
                "headless explicit refresh was not applied");
        Require(server.SetOutputMode(outputs.front().name, 640, 420),
                "headless default refresh change failed");
        Require(server.GetOutputsInfo().front().refresh_mhz == 0,
                "headless unspecified refresh should use the backend default");
        Require(server.SetOutputMode(outputs.front().name, 640, 420, 60000),
                "headless mode change failed");
        OutputPixels output_pixels(server);
        server.AttachControl(control_fd, getppid());
        std::deque<Json> controls;
        std::string relay_pending;
        auto drain = [&] {
            for (auto &message : ReadMessages(relay_fd, relay_pending)) {
                controls.push_back(std::move(message));
            }
        };
        Until(
            server,
            [&] {
                drain();
                return !controls.empty();
            },
            "control Ready missing");
        Require(controls.front().value("event", "") == "ready" &&
                    controls.front().at("session").get<std::uint64_t>(),
                "invalid Ready");
        controls.clear();
        auto grant = [&](Child &child, prism::contracts::WindowRole role, std::uint64_t id) {
            Write(relay_fd, Json({{"event", "grant"},
                                  {"id", id},
                                  {"pid", child.Pid()},
                                  {"role", static_cast<unsigned>(role)}})
                                    .dump() +
                                "\n");
            auto matches = [&](const auto &value) {
                return value.value("event", "") == "registered" &&
                       value.at("id").template get<std::uint64_t>() == id;
            };
            Until(
                server,
                [&] {
                    drain();
                    return std::any_of(controls.begin(), controls.end(), matches);
                },
                "grant acknowledgement missing");
            const auto value = std::find_if(controls.begin(), controls.end(), matches);
            Require(value->at("success").get<bool>(), "client grant rejected");
            controls.erase(value);
            child.Start();
            child.WaitReady();
        };
        Child desktop(server, executable, display, "desktop");
        grant(desktop, prism::contracts::WindowRole::Desktop, 1);
        Require(desktop.ready["width"] == 640 && desktop.ready["height"] == 420,
                "Desktop did not receive trusted Shell geometry");
        Require(compositor->GetWindows().empty(), "trusted Desktop entered ordinary BSP tree");
        Child app(server, executable, display, "app");
        grant(app, prism::contracts::WindowRole::Toplevel, 2);
        Until(
            server,
            [&] {
                return compositor->GetWindows().size() == 1 &&
                       compositor->GetWindows().front()->GetCommittedBounds().width > 0;
            },
            "ordinary App did not map");
        Require(compositor->GetWindows().front()->GetAppId() == "prism.scheduling-app",
                "ordinary/tree role mismatch");
        const bool gpu = app.ready.value("backdrop", false);
        Require(!gles || gpu, "requested GLES effects not supported");
        report["effects_supported"] = gpu;
        auto snapshot = [&] {
            auto state = Status(server);
            state["actual_output_commit_seq"] = ActualCommits(server);
            return state;
        };
        auto quiet = [&](const char *scenario) {
            RunFor(server, 200ms);
            const auto before = snapshot();
            RunFor(server, 1000ms);
            const auto after = snapshot();
            report["scenarios"].push_back(
                {{"name", scenario}, {"before", before}, {"after", after}});
            save();
            Require(before.at("actual_output_commit_seq") == after.at("actual_output_commit_seq"),
                    std::string(scenario) + ": static output keeps committing");
            Require(ScheduleCount(before, "output_commits") ==
                            ScheduleCount(after, "output_commits") &&
                        ScheduleCount(before, "output_buffer_commits") ==
                            ScheduleCount(after, "output_buffer_commits"),
                    std::string(scenario) + ": static output commit counters grow");
            Require(
                EffectCount(before, "rendered_regions") == EffectCount(after, "rendered_regions") &&
                    EffectCount(before, "scene_reorders") == EffectCount(after, "scene_reorders"),
                std::string(scenario) + ": static effects render or reorder");
            Require(EffectCount(before, "update_calls") == EffectCount(after, "update_calls") &&
                        EffectCount(before, "regions_checked") ==
                            EffectCount(after, "regions_checked"),
                    std::string(scenario) + ": static effects keep checking cached regions");
        };
        quiet("initial-static-shell-and-app");
        RunFor(server, 100ms);
        const auto callback_before = snapshot();
        for (int i = 1; i <= 3; ++i) {
            desktop.Send({{"action", "frame"}, {"id", i}});
            desktop.WaitDone(i);
            app.Send({{"action", "frame"}, {"id", i}});
            app.WaitDone(i);
        }
        const auto callback_after = snapshot();
        report["scenarios"].push_back({{"name", "six-callback-only-commits"},
                                       {"before", callback_before},
                                       {"after", callback_after}});
        save();
        Require(ScheduleCount(callback_after, "surface_callback_commits") >=
                    ScheduleCount(callback_before, "surface_callback_commits") + 6,
                "callback-only fixture did not reach production surface commits");
        Require(ScheduleCount(callback_after, "surface_buffer_commits") ==
                    ScheduleCount(callback_before, "surface_buffer_commits"),
                "callback-only fixture unexpectedly attached pixel buffers");
        Require(EffectCount(callback_before, "rendered_regions") ==
                    EffectCount(callback_after, "rendered_regions"),
                "callback-only commits unnecessarily regenerate effects");
        const auto redraw_before = ActualCommits(server);
        app.Send({{"action", "draw"}, {"id", 10}});
        app.WaitDone(10);
        Until(
            server, [&] { return ActualCommits(server) != redraw_before; },
            "draw after callback-only chain did not commit");
        quiet("static-after-callback-chain-and-redraw");
        if (gpu) {
            auto before = snapshot();
            app.Send({{"action", "effects"}, {"id", 20}, {"count", 2}, {"blur", 12}});
            app.WaitAck(20);
            Until(
                server,
                [&] {
                    return EffectCount(Status(server), "rendered_regions") >
                           EffectCount(before, "rendered_regions");
                },
                "effect-only commit did not wake material rendering");
            auto after = snapshot();
            report["scenarios"].push_back(
                {{"name", "effect-only-two-regions"}, {"before", before}, {"after", after}});
            save();
            quiet("two-regions-static-order");
            before = snapshot();
            app.Send({{"action", "effects"}, {"id", 21}, {"count", 2}, {"blur", 6}});
            app.WaitAck(21);
            Until(
                server,
                [&] {
                    return EffectCount(Status(server), "rendered_regions") >
                           EffectCount(before, "rendered_regions");
                },
                "effect-only parameter change did not invalidate material cache");
            after = snapshot();
            report["scenarios"].push_back(
                {{"name", "effect-only-blur-change"}, {"before", before}, {"after", after}});
            save();
            quiet("two-regions-static-after-parameter-change");
            auto no_paint = [&](Child &client, Json command, const char *name,
                                std::uint64_t buffer_commits, bool evaluate = true,
                                bool unchanged_roi = true, bool output_change = true) {
                const auto before = snapshot();
                const auto roi_before = output_pixels.Read(68, 106, 16, 12);
                const int id = command.at("id").get<int>();
                client.Send(command);
                const auto action = command.at("action").get<std::string>();
                if (action == "patch" || action == "frame") {
                    client.WaitDone(id);
                } else {
                    client.WaitAck(id);
                }
                Until(
                    server,
                    [&] {
                        const auto state = Status(server);
                        return (!evaluate || EffectCount(state, "update_calls") >
                                                 EffectCount(before, "update_calls")) &&
                               (!output_change ||
                                ActualCommits(server) != before.at("actual_output_commit_seq"));
                    },
                    std::string(name) + ": commit/effect dependency evaluation did not finish");
                const auto after = snapshot();
                const auto roi_after = output_pixels.Read(68, 106, 16, 12);
                report["scenarios"].push_back({{"name", name},
                                               {"command", command},
                                               {"before", before},
                                               {"after", after},
                                               {"roi_before", MeanRgb(roi_before)},
                                               {"roi_after", MeanRgb(roi_after)}});
                save();
                for (const auto *field : {"capture_pass_attempts", "blur_pass_attempts",
                                          "material_pass_attempts", "rendered_regions"}) {
                    Require(EffectCount(after, field) == EffectCount(before, field),
                            std::string(name) + ": unrelated update repainted " + field);
                }
                Require(ScheduleCount(after, "surface_buffer_commits") ==
                            ScheduleCount(before, "surface_buffer_commits") + buffer_commits,
                        std::string(name) + ": incorrect producer-buffer fixture count");
                if (action == "frame" || action == "metadata") {
                    Require(EffectCount(after, "content_revisions") ==
                                EffectCount(before, "content_revisions"),
                            std::string(name) + ": metadata was recorded as pixel content");
                }
                if (action == "patch" && unchanged_roi) {
                    Require(EffectCount(after, "partial_damage_cache_hits") >
                                EffectCount(before, "partial_damage_cache_hits"),
                            std::string(name) + ": outside logical damage did not advance cached "
                                                "dependency observations");
                }
                if (unchanged_roi) {
                    Require(roi_before == roi_after,
                            std::string(name) + ": unrelated update changed actual glass pixels");
                }
                return std::pair{roi_before, roi_after};
            };
            // These points are outside both complete padded capture extents,
            // including the dependency filter guard, not just outside panels.
            no_paint(desktop, {{"action", "frame"}, {"id", 100}}, "lower-callback-only", 0, false,
                     true, false);
            no_paint(desktop,
                     {{"action", "patch"},
                      {"id", 101},
                      {"x", 500},
                      {"y", 350},
                      {"width", 32},
                      {"height", 32},
                      {"color", 0xff2040c0U}},
                     "fresh-buffer-far-damage-after-callback", 1);
            no_paint(desktop, {{"action", "metadata"}, {"id", 102}, {"kind", "input"}},
                     "lower-input-metadata", 0, false, true, false);
            no_paint(desktop, {{"action", "metadata"}, {"id", 103}, {"kind", "opaque"}},
                     "lower-opaque-metadata", 0, false, true, false);
            no_paint(desktop,
                     {{"action", "patch"},
                      {"id", 104},
                      {"x", 500},
                      {"y", 350},
                      {"width", 32},
                      {"height", 32},
                      {"color", 0xffc04020U}},
                     "fresh-buffer-far-damage-after-metadata", 1);
            no_paint(desktop, {{"action", "sub_create"}, {"id", 105}, {"x", 500}, {"y", 350}},
                     "far-child-first-map", 1);
            no_paint(desktop, {{"action", "sub_patch"}, {"id", 106}, {"color", 0xff20c070U}},
                     "far-child-pixel-change", 1);
            no_paint(desktop, {{"action", "sub_move"}, {"id", 107}, {"x", 520}, {"y", 350}},
                     "far-child-position-change", 0);
            no_paint(desktop, {{"action", "sub_delete"}, {"id", 108}}, "far-child-removal", 0);
            {
                const auto before = snapshot();
                const auto roi_before = output_pixels.Read(68, 106, 16, 12);
                desktop.Send({{"action", "patch"},
                              {"id", 109},
                              {"x", 60},
                              {"y", 100},
                              {"width", 32},
                              {"height", 24},
                              {"color", 0xffea3030U}});
                desktop.WaitDone(109);
                Until(
                    server,
                    [&] {
                        return EffectCount(Status(server), "rendered_regions") >
                                   EffectCount(before, "rendered_regions") &&
                               ActualCommits(server) != before.at("actual_output_commit_seq");
                    },
                    "intersecting partial damage did not repaint and commit glass");
                const auto after = snapshot();
                const auto roi_after = output_pixels.Read(68, 106, 16, 12);
                Require(EffectCount(after, "capture_passes") ==
                                EffectCount(before, "capture_passes") + 1 &&
                            EffectCount(after, "rendered_regions") ==
                                EffectCount(before, "rendered_regions") + 1,
                        "partial damage repainted more than its intersecting region");
                Require(MeanChange(roi_before, roi_after) > 10,
                        "near damage did not change actual glass ROI");
                report["scenarios"].push_back({{"name", "fresh-buffer-near-partial-damage"},
                                               {"before", before},
                                               {"after", after},
                                               {"roi_before", MeanRgb(roi_before)},
                                               {"roi_after", MeanRgb(roi_after)}});
                save();
            }
            {
                const auto before = snapshot();
                const auto roi_before = output_pixels.Read(26, 104, 2, 8);
                // Panel begins at x26, but x23..25 still contributes to its
                // blurred edge through the padded capture/filter footprint.
                desktop.Send({{"action", "patch"},
                              {"id", 110},
                              {"x", 23},
                              {"y", 104},
                              {"width", 2},
                              {"height", 12},
                              {"color", 0xff20e0e0U}});
                desktop.WaitDone(110);
                Until(
                    server,
                    [&] {
                        return EffectCount(Status(server), "rendered_regions") >
                                   EffectCount(before, "rendered_regions") &&
                               ActualCommits(server) != before.at("actual_output_commit_seq");
                    },
                    "padding damage was incorrectly clipped to panel bounds");
                const auto after = snapshot();
                const auto roi_after = output_pixels.Read(26, 104, 2, 8);
                Require(EffectCount(after, "capture_passes") ==
                            EffectCount(before, "capture_passes") + 1,
                        "padding damage did not select exactly one capture region");
                Require(MeanChange(roi_before, roi_after) > 1,
                        "padding damage did not reach actual blurred edge pixels");
                report["scenarios"].push_back({{"name", "damage-in-padding-outside-panel"},
                                               {"before", before},
                                               {"after", after},
                                               {"roi_before", MeanRgb(roi_before)},
                                               {"roi_after", MeanRgb(roi_after)}});
                save();
            }
            quiet("static-after-partial-damage-and-remote-nodes");
            {
                const auto before = snapshot();
                app.Send({{"action", "effects"}, {"id", 111}, {"count", 2}, {"blur", 0}});
                app.WaitAck(111);
                Until(
                    server,
                    [&] {
                        return EffectCount(Status(server), "rendered_regions") >
                               EffectCount(before, "rendered_regions");
                    },
                    "zero-blur fixture material was not installed");
                const auto after = snapshot();
                Require(EffectCount(after, "capture_pass_attempts") ==
                                EffectCount(before, "capture_pass_attempts") &&
                            EffectCount(after, "blur_pass_attempts") ==
                                EffectCount(before, "blur_pass_attempts"),
                        "zero-blur material captured its background");
                Until(
                    server,
                    [&] { return ActualCommits(server) != before.at("actual_output_commit_seq"); },
                    "zero-blur scene was not committed");
                const auto pixels =
                    no_paint(desktop,
                             {{"action", "patch"},
                              {"id", 112},
                              {"x", 60},
                              {"y", 100},
                              {"width", 32},
                              {"height", 24},
                              {"color", 0xff3050eaU}},
                             "zero-blur-frame-independent-of-lower-pixels", 1, true, false);
                Require(MeanChange(pixels.first, pixels.second) > 10,
                        "zero-blur lower damage failed to update the actual client composition");
                app.Send({{"action", "effects"}, {"id", 113}, {"count", 2}, {"blur", 6}});
                app.WaitAck(113);
                Until(
                    server,
                    [&] {
                        return EffectCount(Status(server), "capture_passes") >
                               EffectCount(after, "capture_passes");
                    },
                    "blur restoration did not capture the latest lower pixels");
            }
            quiet("static-after-zero-blur-restoration");
            auto pattern = [&](int degrees, int id) {
                const auto before = snapshot();
                const auto sampling_before = ClientSampling(server, desktop.Pid());
                const auto direct_before = MeanRgb(output_pixels.Read(480, 355, 16, 12));
                desktop.Send({{"action", "rotated_pattern"}, {"id", id}, {"degrees", degrees}});
                desktop.WaitDone(id);
                Until(
                    server,
                    [&] {
                        return EffectCount(Status(server), "capture_passes") >
                                   EffectCount(before, "capture_passes") &&
                               ActualCommits(server) != before.at("actual_output_commit_seq");
                    },
                    "rotated Desktop pattern did not capture and commit");
                const auto direct = MeanRgb(output_pixels.Read(480, 355, 16, 12));
                const auto after = snapshot();
                report["scenarios"].push_back(
                    {{"name", "pattern-readback-diagnostic"},
                     {"degrees", degrees},
                     {"before", before},
                     {"after", after},
                     {"producer", desktop.patterns.at(id)},
                     {"native_sampling_before", sampling_before},
                     {"native_sampling_after", ClientSampling(server, desktop.Pid())},
                     {"app_native_sampling", ClientSampling(server, app.Pid())},
                     {"yellow_rgb_before", direct_before},
                     {"yellow_rgb", direct},
                     {"yellow_mirrored_y_rgb", MeanRgb(output_pixels.Read(480, 53, 16, 12))},
                     {"near_rgb", MeanRgb(output_pixels.Read(68, 106, 16, 12))},
                     {"near_mirrored_y_rgb", MeanRgb(output_pixels.Read(68, 302, 16, 12))},
                     {"blue_rgb", MeanRgb(output_pixels.Read(560, 300, 16, 12))},
                     {"blue_mirrored_y_rgb", MeanRgb(output_pixels.Read(560, 108, 16, 12))}});
                save();
                Require(
                    direct[0] > 180 && direct[1] > 160 && direct[2] < 70,
                    "fixture's native logical-to-buffer rotation mapping is incorrect: degrees=" +
                        std::to_string(degrees) + " yellow_rgb=" + Json(direct).dump());
                return output_pixels.Read(68, 106, 16, 12);
            };
            const auto logical_reference = pattern(0, 130);
            for (const int degrees : {90, 270}) {
                const int id = degrees == 90 ? 131 : 134;
                const auto rotated = pattern(degrees, id);
                Require(MeanChange(logical_reference, rotated) < 2,
                        "capture rotation disagrees with native inverse transform");
                report["scenarios"].push_back({{"name", "asymmetric-pattern-rotation"},
                                               {"degrees", degrees},
                                               {"reference_roi", MeanRgb(logical_reference)},
                                               {"rotated_roi", MeanRgb(rotated)}});
                save();
                const auto name = "rotated-" + std::to_string(degrees) + "-fresh-buffer-far-damage";
                no_paint(desktop,
                         {{"action", "patch"},
                          {"id", id + 1},
                          {"x", 500},
                          {"y", 350},
                          {"width", 32},
                          {"height", 32},
                          {"color", 0xff20c050U}},
                         name.c_str(), 1);
                const auto direct = MeanRgb(output_pixels.Read(506, 356, 16, 12));
                Require(direct[0] < 60 && direct[1] > 160 && direct[2] < 110,
                        "rotated buffer damage updated the wrong native logical location");
                const auto before = snapshot();
                const auto roi_before = output_pixels.Read(68, 106, 16, 12);
                desktop.Send({{"action", "patch"},
                              {"id", id + 2},
                              {"x", 60},
                              {"y", 100},
                              {"width", 32},
                              {"height", 24},
                              {"color", 0xff2030e0U}});
                desktop.WaitDone(id + 2);
                Until(
                    server,
                    [&] {
                        return EffectCount(Status(server), "capture_passes") >
                                   EffectCount(before, "capture_passes") &&
                               ActualCommits(server) != before.at("actual_output_commit_seq");
                    },
                    "rotated near logical damage did not recapture and commit");
                const auto after = snapshot();
                const auto roi_after = output_pixels.Read(68, 106, 16, 12);
                Require(EffectCount(after, "capture_passes") ==
                                EffectCount(before, "capture_passes") + 1 &&
                            MeanChange(roi_before, roi_after) > 10,
                        "rotated partial damage did not select and repaint its actual logical "
                        "glass region");
                report["scenarios"].push_back({{"name", "rotated-near-partial-damage"},
                                               {"degrees", degrees},
                                               {"before", before},
                                               {"after", after},
                                               {"roi_before", MeanRgb(roi_before)},
                                               {"roi_after", MeanRgb(roi_after)}});
                save();
            }
            pattern(0, 137);
            {
                const auto before = snapshot();
                desktop.Send({{"action", "offset"}, {"id", 138}});
                desktop.WaitAck(138);
                Until(
                    server,
                    [&] {
                        return EffectCount(Status(server), "capture_passes") >
                                   EffectCount(before, "capture_passes") &&
                               ActualCommits(server) != before.at("actual_output_commit_seq");
                    },
                    "offset commit did not conservatively invalidate its dependent captures");
                const auto after = snapshot();
                report["scenarios"].push_back({{"name", "offset-only-conservative-content-epoch"},
                                               {"before", before},
                                               {"after", after}});
                save();
                // A mapping epoch forces recapture independently of pixel
                // revision. OFFSET supplies neither a buffer nor pixel damage.
                Require(EffectCount(after, "content_revisions") ==
                                EffectCount(before, "content_revisions") &&
                            EffectCount(after, "metadata_commits") ==
                                EffectCount(before, "metadata_commits") + 1 &&
                            EffectCount(after, "damage_history_fallbacks") >
                                EffectCount(before, "damage_history_fallbacks") &&
                            ScheduleCount(after, "surface_buffer_commits") ==
                                ScheduleCount(before, "surface_buffer_commits"),
                        "offset mapping epoch changed pixel revision, attached a buffer or omitted "
                        "conservative invalidation");
            }
            quiet("static-after-rotation-and-offset");
            before = snapshot();
            desktop.Send({{"action", "draw"}, {"id", 30}});
            desktop.WaitDone(30);
            Until(
                server,
                [&] {
                    return EffectCount(Status(server), "rendered_regions") >
                           EffectCount(before, "rendered_regions");
                },
                "lower Desktop pixel update did not regenerate backdrop");
            quiet("static-after-lower-content-change");
            auto effect_change = [&](Child &client, Json command, const char *name,
                                     std::uint64_t buffer_commits, bool evaluate_only = false,
                                     bool require_commit = true) {
                const auto before = snapshot();
                const int id = command.at("id").get<int>();
                client.Send(command);
                client.WaitAck(id);
                if (evaluate_only) {
                    // Moving this client's local geometry changes its scene,
                    // while unchanged lower content can reuse its backdrop.
                    Until(
                        server,
                        [&] {
                            const auto state = Status(server);
                            return EffectCount(state, "update_calls") >
                                       EffectCount(before, "update_calls") &&
                                   EffectCount(state, "regions_checked") >
                                       EffectCount(before, "regions_checked");
                        },
                        std::string(name) + ": local geometry did not reevaluate effects");
                    if (require_commit) {
                        Until(
                            server,
                            [&] {
                                return ActualCommits(server) !=
                                       before.at("actual_output_commit_seq");
                            },
                            std::string(name) +
                                ": local geometry did not commit the changed client scene");
                    }
                } else {
                    Until(
                        server,
                        [&] {
                            return EffectCount(Status(server), "rendered_regions") >
                                   EffectCount(before, "rendered_regions");
                        },
                        std::string(name) +
                            ": geometry/mapping change did not regenerate backdrop");
                }
                const auto after = snapshot();
                report["scenarios"].push_back(
                    {{"name", name}, {"before", before}, {"after", after}});
                save();
                Require(ScheduleCount(after, "surface_buffer_commits") ==
                            ScheduleCount(before, "surface_buffer_commits") + buffer_commits,
                        std::string(name) +
                            ": unexpected buffer attach during geometry-only fixture");
                quiet((std::string("static-after-") + name).c_str());
            };
            // One child above the opaque Desktop intersects the App's first
            // blur region. Its move/order commits carry no parent pixel state.
            effect_change(desktop, {{"action", "sub_create"}, {"id", 50}}, "subsurface-first-map",
                          1);
            effect_change(desktop, {{"action", "sub_move"}, {"id", 51}},
                          "parent-only-subsurface-position", 0);
            effect_change(desktop, {{"action", "sub_order"}, {"id", 52}, {"above", false}},
                          "parent-only-subsurface-below", 0);
            effect_change(desktop, {{"action", "sub_order"}, {"id", 53}, {"above", true}},
                          "parent-only-subsurface-above", 0);
            effect_change(desktop, {{"action", "sub_unmap"}, {"id", 54}}, "subsurface-unmap", 1);
            effect_change(desktop, {{"action", "sub_map"}, {"id", 55}}, "subsurface-remap", 1);
            const auto original_sampling = ClientSampling(server, app.Pid());
            effect_change(app, {{"action", "geometry"}, {"id", 56}, {"origin", 0}},
                          "xdg-geometry-reserve", 0, true, false);
            const auto reserved_sampling = ClientSampling(server, app.Pid());
            Require(original_sampling == reserved_sampling,
                    "geometry reserve unexpectedly changed the native client sampler");
            const auto committed_size = compositor->GetWindows().front()->GetCommittedBounds();
            effect_change(app, {{"action", "geometry"}, {"id", 57}, {"origin", 4}},
                          "xdg-origin-only", 0, true);
            const auto shifted_size = compositor->GetWindows().front()->GetCommittedBounds();
            Require(ClientSampling(server, app.Pid()) != reserved_sampling,
                    "geometry origin did not move native client sampling");
            Require(committed_size.width == shifted_size.width &&
                        committed_size.height == shifted_size.height,
                    "origin-only XDG fixture changed effective geometry size");
        }
        {
            Pointer pointer(server);
            const auto before = snapshot();
            app.Drain();
            const auto previous_events = app.pointer_events;
            pointer.Absolute(.25, .3);
            pointer.Absolute(.35, .35);
            Until(
                server,
                [&] {
                    app.Drain();
                    return app.pointer_events > previous_events;
                },
                "pointer motion did not reach actual Wayland client");
            Require(Status(server).at("performance").at("pointer_events").get<std::uint64_t>() >
                        before.at("performance").at("pointer_events").get<std::uint64_t>(),
                    "production pointer handler was not exercised");
            app.Send({{"action", "frame"}, {"id", 40}});
            app.WaitDone(40);
            const auto redraw = ActualCommits(server);
            app.Send({{"action", "draw"}, {"id", 41}});
            app.WaitDone(41);
            Until(
                server, [&] { return ActualCommits(server) != redraw; },
                "redraw after pointer wake failed");
            quiet("static-after-pointer-wake-and-redraw");
        }
        if (gpu) {
            // A real trusted Dock overlaps the lower App's cached shadow. Focus
            // changes repaint that frame in-place with the same buffer/texture
            // identity. Only the lower paint content generation can update the
            // Dock's otherwise unchanged capture dependency.
            auto theme = prism::test::WmThemeFixture(2);
            theme.focused.shadow = {240, 25, 25, 235};
            theme.normal.shadow = {25, 25, 240, 235};
            Require(server.InstallTheme(theme).success, "paint-chain theme fixture rejected");
            Child other(server, executable, display, "app");
            grant(other, prism::contracts::WindowRole::Toplevel, 3);
            Until(
                server, [&] { return compositor->GetWindows().size() == 2; },
                "paint-chain second ordinary App did not map");
            Child dock(server, executable, display, "dock");
            grant(dock, prism::contracts::WindowRole::Dock, 4);
            Require(dock.ready.at("width") == 620 && dock.ready.at("height") == 100 &&
                        compositor->GetWindows().size() == 2,
                    "paint-chain trusted Dock geometry/tree contract failed");
            const auto start = snapshot();
            dock.Send({{"action", "effects"},
                       {"id", 120},
                       {"count", 1},
                       {"blur", 12},
                       {"x", 12},
                       {"y", 0},
                       {"width", 100},
                       {"height", 32}});
            dock.WaitAck(120);
            Until(
                server,
                [&] {
                    return EffectCount(Status(server), "capture_passes") >
                           EffectCount(start, "capture_passes");
                },
                "upper Dock capture did not initialize");
            quiet("paint-chain-static-before-focus");
            const auto before = snapshot();
            const auto roi_before = output_pixels.Read(70, 322, 32, 6);
            Require(Command(server, "focus left").value("status", "") == "ok",
                    "paint-chain directional focus failed");
            Until(
                server,
                [&] {
                    return EffectCount(Status(server), "capture_passes") >
                               EffectCount(before, "capture_passes") &&
                           ActualCommits(server) != before.at("actual_output_commit_seq");
                },
                "lower cached paint generation did not propagate to upper capture");
            const auto after = snapshot();
            const auto roi_after = output_pixels.Read(70, 322, 32, 6);
            Require(MeanChange(roi_before, roi_after) > 5,
                    "lower cached shadow recolor did not reach actual upper Dock glass");
            Require(ScheduleCount(after, "surface_buffer_commits") ==
                        ScheduleCount(before, "surface_buffer_commits"),
                    "paint-chain focus unexpectedly changed client buffers");
            report["scenarios"].push_back({{"name", "intersecting-cached-paint-generation"},
                                           {"before", before},
                                           {"after", after},
                                           {"roi_before", MeanRgb(roi_before)},
                                           {"roi_after", MeanRgb(roi_after)}});
            save();
            quiet("paint-chain-static-after-focus");
        }
        report["passed"] = true;
        save();
        std::cout
            << "Production WM scheduling: static output, callback-only progress, redraw recovery, "
            << (gpu ? "partial-damage ROI/metadata/two-region order/subsurface geometry/cached "
                      "paint propagation, "
                    : "GLES effects skipped, ")
            << "Wayland pointer delivery passed.\n";
    } catch (const std::exception &error) {
        report["passed"] = false;
        report["error"] = error.what();
        save();
        server.Stop();
        std::filesystem::remove_all(directory);
        throw;
    }
    server.Stop();
    std::filesystem::remove_all(directory);
    return 0;
}

// Create the PWC endpoint before fork and retain its controller in the actual
// parent. A small test-only relay carries fixture requests; production WM still
// authenticates SO_PEERCRED and getppid, with no authentication bypass.
int Supervisor(const std::string &executable, bool gles, const std::filesystem::path &report_path)
{
    int control[2], relay[2];
    Require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0,
            "supervisor control socketpair failed");
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, relay) < 0) {
        close(control[0]);
        close(control[1]);
        throw std::runtime_error("supervisor relay socketpair failed");
    }
    const auto supervisor_pid = getpid();
    const auto child = fork();
    Require(child >= 0, "WM fixture fork failed");
    if (!child) {
        close(control[0]);
        close(relay[0]);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != supervisor_pid) {
            _exit(1);
        }
        int result = 1;
        try {
            result = RunScenarios(executable, gles, report_path, control[1], relay[1]);
        } catch (const std::exception &error) {
            std::cerr << "render scheduling probe failed: " << error.what() << '\n';
        }
        close(relay[1]);
        std::cout.flush();
        std::cerr.flush();
        _exit(result);
    }
    close(control[1]);
    close(relay[1]);
    prism::launch::Stream controller(control[0], prism::launch::ControlFrameSize);
    std::string relay_pending;
    std::uint64_t session{};
    try {
        for (;;) {
            int status{};
            const auto exited = waitpid(child, &status, WNOHANG);
            if (exited == child) {
                close(relay[0]);
                return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            }
            Require(exited == 0 || (exited < 0 && errno == EINTR), "WM fixture waitpid failed");
            controller.Flush();
            for (const auto &bytes : controller.Receive()) {
                const auto message = prism::launch::DecodeControl(bytes);
                if (message.type == prism::launch::ControlType::Ready) {
                    Require(!session && message.permit.pid == static_cast<std::uint32_t>(child),
                            "invalid supervisor Ready");
                    session = message.permit.session;
                    Write(relay[0], Json({{"event", "ready"}, {"session", session}}).dump() + "\n");
                } else if (message.type == prism::launch::ControlType::Registered) {
                    Write(relay[0], Json({{"event", "registered"},
                                          {"id", message.permit.request.value},
                                          {"success", message.success}})
                                            .dump() +
                                        "\n");
                }
            }
            for (const auto &request : ReadMessages(relay[0], relay_pending)) {
                Require(session && request.value("event", "") == "grant",
                        "invalid supervisor relay request");
                prism::launch::ControlMessage message;
                message.type = prism::launch::ControlType::Grant;
                const auto id = request.at("id").get<std::uint64_t>();
                message.permit.session = session;
                message.permit.request = {id};
                message.permit.instance = {id + 100};
                message.permit.pid = request.at("pid").get<pid_t>();
                message.permit.role =
                    static_cast<prism::contracts::WindowRole>(request.at("role").get<unsigned>());
                message.permit.expires_ns = prism::launch::MonotonicNs() + 60000000000ULL;
                prism::launch::RandomBytes(message.permit.token);
                Require(controller.Queue(prism::launch::EncodeControl(message)),
                        "supervisor grant queue failed");
            }
            controller.Flush();
            pollfd descriptors[] = {
                {controller.Fd(),
                 static_cast<short>(POLLIN | (controller.WantsWrite() ? POLLOUT : 0)), 0},
                {relay[0], POLLIN, 0}};
            const auto result = poll(descriptors, 2, 50);
            Require(result >= 0 || errno == EINTR, "supervisor poll failed");
        }
    } catch (...) {
        kill(child, SIGKILL);
        int status{};
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        close(relay[0]);
        throw;
    }
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 5 && std::string_view(argv[1]) == "--client") {
        const int channel = std::stoi(argv[4]);
        try {
            RawClient client(channel, argv[3]);
            return client.Run(argv[2]);
        } catch (const std::exception &error) {
            try {
                Write(channel, Json({{"event", "error"}, {"detail", error.what()}}).dump() + "\n");
            } catch (...) {
            }
            return 1;
        }
    }
    try {
        bool gles = false;
        std::filesystem::path report;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--gles") {
                gles = true;
            } else if (std::string_view(argv[i]) == "--report" && i + 1 < argc) {
                report = argv[++i];
            } else {
                throw std::runtime_error("Usage: render_scheduling_probe [--gles] [--report PATH]");
            }
        }
        return Supervisor(std::filesystem::canonical(argv[0]).string(), gles, report);
    } catch (const std::exception &error) {
        std::cerr << "render scheduling probe failed: " << error.what() << '\n';
        return 1;
    }
}
