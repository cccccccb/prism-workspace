// Exercises the public SDK client against real Wayland transport and wlroots
// surfaces. The selectable effect manager records transactions without GPU work.
#include "prism-surface-effects-server.h"
#include "prism/contracts/contour.hpp"
#include "prism/platform/wayland_window.hpp"

extern "C" {
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <wayland-server-core.h>

namespace {
using prism::contracts::Contour;
using prism::contracts::SurfaceEffectRegion;
using prism::platform::WaylandWindow;

struct Snapshot {
    unsigned manager_version{}, effect_version{}, creates{}, clears{}, rectangles{}, contours{},
        commits{};
    std::vector<SurfaceEffectRegion> pending, current;
    std::vector<std::uint8_t> last_payload;

    bool operator==(const Snapshot &) const = default;
};

class Server {
    struct Toplevel {
        wlr_xdg_toplevel *native{};
        wl_listener commit{}, destroy{};
    };

    struct Effect {
        Server *server{};
        wlr_surface *surface{};
        wl_listener commit{}, destroy{};
    };

    wl_display *display_{};
    wlr_renderer *renderer_{};
    wl_listener shell_listener_{};
    std::uint32_t version_{};
    bool contour_supported_{};
    std::thread thread_;
    std::atomic<bool> stop_{};
    std::mutex mutex_;
    Snapshot snapshot_;

    static void Destroy(wl_client *, wl_resource *resource)
    {
        wl_resource_destroy(resource);
    }

    static void ToplevelCommit(wl_listener *listener, void *)
    {
        auto *state = reinterpret_cast<Toplevel *>(reinterpret_cast<char *>(listener) -
                                                   offsetof(Toplevel, commit));
        if (state->native->base->initial_commit) {
            wlr_xdg_toplevel_set_size(state->native, 160, 90);
        }
    }

    static void ToplevelGone(wl_listener *listener, void *)
    {
        auto *state = reinterpret_cast<Toplevel *>(reinterpret_cast<char *>(listener) -
                                                   offsetof(Toplevel, destroy));
        wl_list_remove(&state->commit.link);
        wl_list_remove(&state->destroy.link);
        delete state;
    }

    static void NewToplevel(wl_listener *, void *data)
    {
        auto *native = static_cast<wlr_xdg_toplevel *>(data);
        auto *state = new Toplevel;
        state->native = native;
        state->commit.notify = ToplevelCommit;
        state->destroy.notify = ToplevelGone;
        wl_signal_add(&native->base->surface->events.commit, &state->commit);
        wl_signal_add(&native->events.destroy, &state->destroy);
    }

    static void EffectCommit(wl_listener *listener, void *)
    {
        auto *effect = reinterpret_cast<Effect *>(reinterpret_cast<char *>(listener) -
                                                  offsetof(Effect, commit));
        std::lock_guard lock(effect->server->mutex_);
        auto &snapshot = effect->server->snapshot_;
        ++snapshot.commits;
        snapshot.current = snapshot.pending;
    }

    static void EffectSurfaceGone(wl_listener *listener, void *)
    {
        auto *effect = reinterpret_cast<Effect *>(reinterpret_cast<char *>(listener) -
                                                  offsetof(Effect, destroy));
        wl_list_remove(&effect->commit.link);
        wl_list_remove(&effect->destroy.link);
        effect->surface = nullptr;
    }

    static void EffectGone(wl_resource *resource)
    {
        auto *effect = static_cast<Effect *>(wl_resource_get_user_data(resource));
        if (effect->surface) {
            wl_list_remove(&effect->commit.link);
            wl_list_remove(&effect->destroy.link);
        }
        delete effect;
    }

    static void Clear(wl_client *, wl_resource *resource)
    {
        auto *effect = static_cast<Effect *>(wl_resource_get_user_data(resource));
        std::lock_guard lock(effect->server->mutex_);
        auto &snapshot = effect->server->snapshot_;
        ++snapshot.clears;
        snapshot.pending.clear();
    }

    static void AddRectangle(wl_client *, wl_resource *resource, wl_fixed_t x, wl_fixed_t y,
                             wl_fixed_t width, wl_fixed_t height, wl_fixed_t radius,
                             wl_fixed_t blur)
    {
        auto *effect = static_cast<Effect *>(wl_resource_get_user_data(resource));
        const SurfaceEffectRegion region{{wl_fixed_to_double(x), wl_fixed_to_double(y),
                                          wl_fixed_to_double(width), wl_fixed_to_double(height)},
                                         wl_fixed_to_double(radius),
                                         wl_fixed_to_double(blur)};
        prism::contracts::ValidateSurfaceEffectRegion(region);

        std::lock_guard lock(effect->server->mutex_);
        auto &snapshot = effect->server->snapshot_;
        ++snapshot.rectangles;
        snapshot.pending.push_back(region);
    }

    static void AddContour(wl_client *, wl_resource *resource, wl_fixed_t blur, wl_array *payload)
    {
        auto *effect = static_cast<Effect *>(wl_resource_get_user_data(resource));
        assert(effect->server->contour_supported_ && payload && payload->data);
        const auto bytes =
            std::span(static_cast<const std::uint8_t *>(payload->data), payload->size);
        auto contour = prism::contracts::DecodeContour(bytes);
        SurfaceEffectRegion region{prism::contracts::ContourBounds(contour), 0,
                                   wl_fixed_to_double(blur), std::move(contour)};
        prism::contracts::ValidateSurfaceEffectRegion(region);

        std::lock_guard lock(effect->server->mutex_);
        auto &snapshot = effect->server->snapshot_;
        ++snapshot.contours;
        snapshot.last_payload.assign(bytes.begin(), bytes.end());
        snapshot.pending.push_back(std::move(region));
    }

    static void GetEffect(wl_client *client, wl_resource *manager, std::uint32_t id,
                          wl_resource *surface_resource)
    {
        auto *server = static_cast<Server *>(wl_resource_get_user_data(manager));
        auto *surface = wlr_surface_from_resource(surface_resource);
        const auto version = wl_resource_get_version(manager);
        auto *resource =
            wl_resource_create(client, &prism_surface_effect_v1_interface, version, id);
        assert(resource);
        auto *effect = new Effect;
        effect->server = server;
        effect->surface = surface;
        effect->commit.notify = EffectCommit;
        effect->destroy.notify = EffectSurfaceGone;
        wl_signal_add(&surface->events.commit, &effect->commit);
        wl_signal_add(&surface->events.destroy, &effect->destroy);
        static const struct prism_surface_effect_v1_interface implementation{
            Destroy, Clear, AddRectangle, AddContour};
        wl_resource_set_implementation(resource, &implementation, effect, EffectGone);

        std::lock_guard lock(server->mutex_);
        server->snapshot_.effect_version = version;
        ++server->snapshot_.creates;
    }

    static void Bind(wl_client *client, void *data, std::uint32_t version, std::uint32_t id)
    {
        auto *server = static_cast<Server *>(data);
        auto *resource = wl_resource_create(client, &prism_surface_effect_manager_v1_interface,
                                            std::min(version, server->version_), id);
        assert(resource);
        static const struct prism_surface_effect_manager_v1_interface implementation{Destroy,
                                                                                     GetEffect};
        wl_resource_set_implementation(resource, &implementation, server, nullptr);
        prism_surface_effect_manager_v1_send_capabilities(resource, 1);
        if (version >= 2) {
            prism_surface_effect_manager_v1_send_contour_capabilities(resource,
                                                                      server->contour_supported_);
        }

        std::lock_guard lock(server->mutex_);
        server->snapshot_.manager_version = version;
    }

    void Run()
    {
        auto *loop = wl_display_get_event_loop(display_);
        while (!stop_) {
            assert(wl_event_loop_dispatch(loop, 20) >= 0);
            wl_display_flush_clients(display_);
        }
    }

public:
    Server(const char *socket, std::uint32_t version, bool contour_supported)
        : version_(version), contour_supported_(contour_supported)
    {
        display_ = wl_display_create();
        renderer_ = wlr_pixman_renderer_create();
        // Pixman has no DRM device: only SHM is needed for this transport fixture.
        // Full display initialization also advertises DMA-BUF and requires DRM.
        assert(display_ && renderer_ && wlr_renderer_init_wl_shm(renderer_, display_));
        assert(wlr_compositor_create(display_, 5, renderer_));
        auto *shell = wlr_xdg_shell_create(display_, 3);
        assert(shell);
        shell_listener_.notify = NewToplevel;
        wl_signal_add(&shell->events.new_toplevel, &shell_listener_);
        assert(wl_global_create(display_, &prism_surface_effect_manager_v1_interface, version_,
                                this, Bind));
        assert(wl_display_add_socket(display_, socket) == 0);

        thread_ = std::thread(&Server::Run, this);
    }

    ~Server()
    {
        stop_ = true;
        thread_.join();
        wl_display_destroy_clients(display_);
        wl_list_remove(&shell_listener_.link);
        wlr_renderer_destroy(renderer_);
        wl_display_destroy(display_);
    }

    Snapshot Inspect()
    {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }
};

Contour Neck(double neck_shift = 0)
{
    return {{{24.25 + neck_shift, 7.75},
             {34.25 + neck_shift, 7.75},
             {34.25 + neck_shift, 15.75},
             {48.25, 15.75},
             {48.25, 43.75},
             {10.25, 43.75},
             {10.25, 15.75},
             {24.25 + neck_shift, 15.75}}};
}

SurfaceEffectRegion ContourRegion(double neck_shift = 0)
{
    auto contour = Neck(neck_shift);
    return {prism::contracts::ContourBounds(contour), 0, 4, std::move(contour)};
}

void Paint(void *pixels, int, int height, int stride)
{
    std::memset(pixels, 0, std::size_t(height) * stride);
}

void Sync(WaylandWindow &window)
{
    assert(window.Display() && wl_display_roundtrip(window.Display()) >= 0);
}

void Commit(WaylandWindow &window)
{
    assert(window.Pump(0));
    Sync(window);
    assert(!window.SurfaceStatePending());
}

void Rejected(WaylandWindow &window, Server &server, std::span<const SurfaceEffectRegion> regions)
{
    const auto before = server.Inspect();
    const bool state_pending = window.SurfaceStatePending();
    bool rejected = false;
    try {
        window.SetSurfaceEffects(regions);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);

    Sync(window);
    assert(server.Inspect() == before);
    assert(window.SurfaceStatePending() == state_pending);
}

void Verify(std::uint32_t version, bool contour_supported)
{
    const bool can_contour = version >= 2 && contour_supported;
    Server server("wayland-effects-client-test", version, contour_supported);
    WaylandWindow window;
    window.SetPaintHandler(Paint);
    assert(
        window.Open("wayland-effects-client-test", "effects.fixture", "Effects fixture", 160, 90));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!window.IsMapped()) {
        assert(std::chrono::steady_clock::now() < deadline);
        assert(window.Pump(10));
    }
    Sync(window);
    assert(server.Inspect().manager_version == version);

    // The client prepares the complete list, negotiates geometry capability,
    // and sends pending requests without committing or rendering by itself.
    const SurfaceEffectRegion rectangle{{2.251, 3.75, 80.001, 48}, 6, 4};
    const std::array requested{rectangle, ContourRegion()};
    window.SetSurfaceEffects(requested);
    Sync(window);
    auto snapshot = server.Inspect();
    assert(snapshot.creates == 1 && snapshot.effect_version == version);
    assert(snapshot.clears == 1 && snapshot.rectangles == 1);
    assert(snapshot.contours == unsigned(can_contour));
    assert(snapshot.pending.size() == (can_contour ? 2u : 1u) && snapshot.current.empty());
    assert(!snapshot.pending.front().contour);
    assert(snapshot.pending.front().bounds.x == wl_fixed_to_double(wl_fixed_from_double(2.251)));
    if (can_contour) {
        assert(prism::contracts::DecodeContour(snapshot.last_payload) == Neck());
    }
    Commit(window);
    snapshot = server.Inspect();
    assert(snapshot.current == snapshot.pending && snapshot.commits == 1);

    // Cache equality compares the actual sent list, including fixed-point
    // rectangle values and contour vertices; unsupported contours stay omitted.
    window.SetSurfaceEffects(requested);
    Sync(window);
    assert(server.Inspect() == snapshot && !window.SurfaceStatePending());
    const std::array changed{rectangle, ContourRegion(2)};
    assert(changed[1].bounds.x == requested[1].bounds.x &&
           changed[1].bounds.width == requested[1].bounds.width);
    window.SetSurfaceEffects(changed);
    Sync(window);
    if (can_contour) {
        const auto pending = server.Inspect();
        assert(pending.clears == snapshot.clears + 1 && pending.contours == snapshot.contours + 1);
        assert(pending.current == snapshot.current && pending.pending != snapshot.pending);
        assert(prism::contracts::DecodeContour(pending.last_payload) == Neck(2));
        Commit(window);
        snapshot = server.Inspect();
    } else {
        assert(server.Inspect() == snapshot && !window.SurfaceStatePending());
    }

    // Each rejected mixed transaction contains a valid first region. This
    // catches implementations that clear/add before validating the later one.
    auto bad_contour = ContourRegion();
    bad_contour.contour->points[1] = bad_contour.contour->points[0];
    const std::array invalid_contour{rectangle, bad_contour};
    Rejected(window, server, invalid_contour);
    auto bad_rectangle = rectangle;
    bad_rectangle.bounds.width = -1;
    const std::array invalid_rectangle{ContourRegion(), bad_rectangle};
    Rejected(window, server, invalid_rectangle);
    auto tiny_rectangle = rectangle;
    tiny_rectangle.bounds.width = 0.001;
    const std::array invalid_fixed{ContourRegion(), tiny_rectangle};
    Rejected(window, server, invalid_fixed);
    auto bad_blur = ContourRegion();
    bad_blur.blur_radius = 49;
    const std::array invalid_blur{rectangle, bad_blur};
    Rejected(window, server, invalid_blur);
    std::vector<SurfaceEffectRegion> excessive(9, ContourRegion());
    Rejected(window, server, excessive);

    // Combined geometry consumes the same eight-region budget. The limit is
    // checked before capability filtering, including on a version 1 server.
    std::array<SurfaceEffectRegion, 8> maximum;
    maximum.fill(rectangle);
    maximum.back() = ContourRegion();
    window.SetSurfaceEffects(maximum);
    Sync(window);
    assert(server.Inspect().pending.size() == (can_contour ? 8u : 7u));
    Commit(window);
    snapshot = server.Inspect();

    // Unsupported contour-only lists must clear an earlier rectangle, rather
    // than returning early and leaving its old backdrop active.
    const std::array only_contour{ContourRegion(2)};
    window.SetSurfaceEffects(only_contour);
    Sync(window);
    assert(server.Inspect().clears == snapshot.clears + 1);
    assert(server.Inspect().current == snapshot.current);
    Commit(window);
    assert(server.Inspect().current.size() == unsigned(can_contour));

    window.SetSurfaceEffects({});
    Commit(window);
    assert(server.Inspect().pending.empty() && server.Inspect().current.empty());
    snapshot = server.Inspect();
    window.SetSurfaceEffects({});
    Sync(window);
    assert(server.Inspect() == snapshot && !window.SurfaceStatePending());
    window.Close();
}
} // namespace

int main()
{
    char path[] = "/tmp/prism-effects-client.XXXXXX";
    assert(mkdtemp(path));
    const auto *old_runtime = std::getenv("XDG_RUNTIME_DIR");
    const bool had_runtime = old_runtime != nullptr;
    const std::string old_value = old_runtime ? old_runtime : "";
    assert(setenv("XDG_RUNTIME_DIR", path, 1) == 0);
    Verify(1, true);
    Verify(2, false);
    Verify(2, true);
    if (had_runtime) {
        assert(setenv("XDG_RUNTIME_DIR", old_value.c_str(), 1) == 0);
    } else {
        assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    }
    std::filesystem::remove_all(path);
    std::puts("Actual WaylandWindow effect negotiation/cache/transaction cases passed");
}
