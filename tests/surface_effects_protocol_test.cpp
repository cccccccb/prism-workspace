#include "prism-surface-effects-client.h"
#include "prism/contracts/contour.hpp"
#include "prism/wm/surface_effects.hpp"
#include "prism/wm/xdg_view.hpp"

extern "C" {
#include <drm_fourcc.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_xdg_shell.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include <wayland-server-core.h>

namespace {
using namespace prism;

void Require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

contracts::Contour Neck(double dx = 0)
{
    contracts::Contour result{{{24.25, 7.75},
                               {34.25, 7.75},
                               {34.25, 15.75},
                               {48.25, 15.75},
                               {48.25, 43.75},
                               {10.25, 43.75},
                               {10.25, 15.75},
                               {24.25, 15.75}}};
    for (auto &point : result.points) {
        point.x += dx;
    }
    return result;
}

struct Pixels {
    int x{}, y{}, width{}, height{};
    std::vector<std::array<std::uint8_t, 4>> values;

    const std::array<std::uint8_t, 4> &At(int global_x, int global_y) const
    {
        const int local_x = global_x - x;
        const int local_y = global_y - y;
        Require(local_x >= 0 && local_y >= 0 && local_x < width && local_y < height,
                "Requested pixel outside published effect buffer");
        return values[std::size_t(local_y) * width + local_x];
    }
};

void PrintPixel(const char *label, const Pixels &pixels, int x, int y)
{
    const auto &value = pixels.At(x, y);
    std::printf("%s global(%d,%d) RGBA=(%u,%u,%u,%u)\n", label, x, y, unsigned(value[0]),
                unsigned(value[1]), unsigned(value[2]), unsigned(value[3]));
}

class Fixture {
public:
    struct SurfaceListener {
        wl_listener listener{};
        Fixture *owner{};
    };

    struct Completion {
        bool done{};
    };

    struct ContentListener {
        wl_listener commit{}, destroy{};
        Fixture *owner{};
        wlr_surface *surface{};
    };

    struct SharedBuffer {
        wl_buffer *native{};
        void *pixels{MAP_FAILED};
        std::size_t size{};

        ~SharedBuffer()
        {
            if (native) {
                wl_buffer_destroy(native);
            }
            if (pixels != MAP_FAILED) {
                munmap(pixels, size);
            }
        }
    };

    struct Layer {
        wl_surface *client{};
        wlr_surface *surface{};
        prism_surface_effect_v1 *effect{};
        wlr_scene_tree *tree{}, *content{};
        SharedBuffer *buffer{};
        int width{}, height{};
    };

    explicit Fixture(bool gpu, std::uint32_t version = 2) : requested_version(version)
    {
        server = wl_display_create();
        Require(server, "Cannot create standalone Wayland server");
        if (gpu) {
            backend = wlr_headless_backend_create(wl_display_get_event_loop(server));
            Require(backend, "Cannot create isolated headless backend");
            render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
            Require(render_fd >= 0, "Cannot open real GPU render node");
            renderer = wlr_gles2_renderer_create_with_drm_fd(render_fd);
            Require(renderer, "Cannot initialize real GLES renderer");
            allocator = wlr_allocator_autocreate(backend, renderer);
            Require(allocator, "Cannot initialize GPU allocator");
        } else {
            renderer = wlr_pixman_renderer_create();
            Require(renderer, "Cannot initialize pixman renderer");
        }

        compositor = wlr_compositor_create(server, 5, renderer);
        Require(compositor, "Cannot create real wlroots compositor global");
        Require(wlr_shm_create_with_renderer(server, 1, renderer), "Cannot create SHM global");
        surface_listener.owner = this;
        surface_listener.listener.notify = NewSurface;
        wl_list_init(&surface_listener.listener.link);
        wl_signal_add(&compositor->events.new_surface, &surface_listener.listener);

        scene = wlr_scene_create();
        Require(scene, "Cannot create isolated scene");
        const float red[4]{1, 0, 0, 1};
        const float blue[4]{0, 0, 1, 1};
        const float green[4]{0, 1, 0, 1};
        lower_top = wlr_scene_rect_create(&scene->tree, 128, 24, red);
        lower_bottom = wlr_scene_rect_create(&scene->tree, 128, 104, blue);
        wlr_scene_node_set_position(&lower_bottom->node, 0, 24);
        view.scene_tree = wlr_scene_tree_create(&scene->tree);
        wlr_scene_rect_create(view.scene_tree, 128, 128, green);
        view.width = view.height = 128;
        view.shell_role = 2;

        effects = std::make_unique<wm::SurfaceEffects>(server, renderer, allocator);
        Require(effects->Supported() == gpu, "Backdrop capability does not match renderer");
        effects->Update(scene, {}, nullptr, nullptr);

        int sockets[2];
        Require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0,
                "Cannot create private Wayland connection");
        Require(wl_client_create(server, sockets[0]), "Cannot create server client");
        client = wl_display_connect_to_fd(sockets[1]);
        Require(client, "Cannot create client display");
        registry = wl_display_get_registry(client);
        static const wl_registry_listener registry_events{Global, GlobalRemove};
        wl_registry_add_listener(registry, &registry_events, this);
        Require(Sync(), "Registry discovery failed");
        Require(client_compositor && manager, "Missing compositor/effect globals");
        Require(Sync(), "Capability delivery failed");
        CreateSurface();
    }

    ~Fixture()
    {
        shared_buffers.clear();
        if (client) {
            wl_display_disconnect(client);
        }
        if (server) {
            wl_display_destroy_clients(server);
        }
        effects.reset();
        if (scene) {
            wlr_scene_node_destroy(&scene->tree.node);
        }
        wl_list_remove(&surface_listener.listener.link);
        if (allocator) {
            wlr_allocator_destroy(allocator);
        }
        if (renderer) {
            wlr_renderer_destroy(renderer);
        }
        if (backend) {
            wlr_backend_destroy(backend);
        }
        if (server) {
            wl_display_destroy(server);
        }
        if (render_fd >= 0) {
            close(render_fd);
        }
    }

    bool Sync()
    {
        Completion completion;
        auto *callback = wl_display_sync(client);
        static const wl_callback_listener done{Done};
        wl_callback_add_listener(callback, &done, &completion);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!completion.done && !wl_display_get_error(client)) {
            Require(std::chrono::steady_clock::now() < deadline, "Wayland sync timeout");
            const int flushed = wl_display_flush(client);
            if (flushed < 0 && errno != EAGAIN) {
                return false;
            }
            Require(wl_event_loop_dispatch(wl_display_get_event_loop(server), 1) >= 0,
                    "Server dispatch failed");
            wl_display_flush_clients(server);

            pollfd input{wl_display_get_fd(client), POLLIN, 0};
            const int ready = poll(&input, 1, 5);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            Require(ready >= 0, "Client poll failed");
            if (ready && wl_display_dispatch(client) < 0) {
                return false;
            }
        }
        return completion.done && !wl_display_get_error(client);
    }

    void CreateSurface()
    {
        surface = wl_compositor_create_surface(client_compositor);
        effect = prism_surface_effect_manager_v1_get_surface_effect(manager, surface);
        Require(Sync(), "Surface/effect creation failed");
        Require(server_surface, "Server did not create a real surface");
        base.surface = server_surface;
        toplevel.base = &base;
        view.toplevel = &toplevel;
        view.mapped = view.visible = true;
    }

    void AddRectangle(double x = 10.25, double y = 7.75, double width = 38, double height = 36,
                      double blur = 4)
    {
        prism_surface_effect_v1_add_region(
            effect, wl_fixed_from_double(x), wl_fixed_from_double(y), wl_fixed_from_double(width),
            wl_fixed_from_double(height), 0, wl_fixed_from_double(blur));
    }

    void AddPayload(std::vector<std::uint8_t> &bytes, double blur = 4)
    {
        wl_array payload{bytes.size(), bytes.size(), bytes.data()};
        prism_surface_effect_v1_add_contour(effect, wl_fixed_from_double(blur), &payload);
    }

    void AddContour(const contracts::Contour &contour, double blur = 4)
    {
        auto bytes = contracts::EncodeContour(contour);
        AddPayload(bytes, blur);
    }

    void Commit()
    {
        wl_surface_commit(surface);
        Require(Sync(), "Surface commit failed");
    }

    wm::SurfaceEffects::UpdateResult Update(const contracts::ThemeSnapshot *theme = nullptr,
                                            std::span<const wm::SurfaceEffects::Target> popups = {})
    {
        if (!view.mapped) {
            return effects->Update(scene, {}, nullptr, theme, popups);
        }
        std::array views{&view};
        return effects->Update(scene, views, &view, theme, popups);
    }

    Layer CreateLayer(wlr_scene_tree *parent, int x, int y, int width, int height,
                      std::uint32_t color, int inset_x = 0, int inset_y = 0)
    {
        const auto previous = created_surfaces.size();
        Layer layer;
        layer.client = wl_compositor_create_surface(client_compositor);
        layer.effect = prism_surface_effect_manager_v1_get_surface_effect(manager, layer.client);
        Require(Sync() && created_surfaces.size() == previous + 1,
                "Cannot create isolated target surface");
        layer.surface = created_surfaces.back();
        layer.width = width;
        layer.height = height;
        layer.buffer = CreateBuffer(width, height, color);
        layer.tree = wlr_scene_tree_create(parent);
        Require(layer.tree, "Cannot create target scene tree");
        wlr_scene_node_set_position(&layer.tree->node, x, y);
        layer.content = wlr_scene_tree_create(layer.tree);
        Require(layer.content && wlr_scene_surface_create(layer.content, layer.surface),
                "Cannot create target surface scene leaf");
        // Mirror xdg scene compensation for positive explicit window geometry.
        wlr_scene_node_set_position(&layer.content->node, -inset_x, -inset_y);
        Repaint(layer, 0, 0, width, height, color);
        wlr_surface_map(layer.surface);
        return layer;
    }

    void Repaint(Layer &layer, int x, int y, int width, int height, std::uint32_t color)
    {
        auto *pixels = static_cast<std::uint32_t *>(layer.buffer->pixels);
        for (int row = y; row < y + height; ++row) {
            std::fill_n(pixels + std::size_t(row) * layer.width + x, width, color);
        }
        wl_surface_attach(layer.client, layer.buffer->native, 0, 0);
        wl_surface_damage_buffer(layer.client, x, y, width, height);
        wl_surface_commit(layer.client);
        Require(Sync(), "Target source pixels did not commit");
    }

    void AddLayerContour(const Layer &layer, const contracts::Contour &contour, double blur = 4)
    {
        auto bytes = contracts::EncodeContour(contour);
        wl_array payload{bytes.size(), bytes.size(), bytes.data()};
        prism_surface_effect_v1_add_contour(layer.effect, wl_fixed_from_double(blur), &payload);
        wl_surface_commit(layer.client);
        Require(Sync(), "Target contour did not commit");
    }

    void ExpectError(std::uint32_t expected)
    {
        Require(!Sync(), "Malformed request unexpectedly succeeded");
        Require(wl_display_get_error(client) == EPROTO, "Expected a Wayland protocol error");
        const wl_interface *interface{};
        std::uint32_t object{};
        const auto error = wl_display_get_protocol_error(client, &interface, &object);
        Require(error == expected && interface &&
                    std::strcmp(interface->name, "prism_surface_effect_v1") == 0,
                "Protocol failure used the wrong error code/interface");
    }

    Pixels ReadEffect(std::size_t index = 0, wlr_surface *target = nullptr)
    {
        const auto nodes = effects->PresentationNodes(target ? target : server_surface);
        Require(index < nodes.size(), "No published effect buffer");
        auto *tree = wlr_scene_tree_from_node(nodes[index]);
        Require(!wl_list_empty(&tree->children), "Effect tree is empty");
        wlr_scene_node *node{};
        node = wl_container_of(tree->children.next, node, link);
        auto *buffer = wlr_scene_buffer_from_node(node);
        Require(buffer && buffer->buffer, "Effect scene node has no real buffer");
        auto *texture = wlr_texture_from_buffer(renderer, buffer->buffer);
        Require(texture, "Cannot import published effect for readback");

        Pixels pixels;
        wlr_scene_node_coords(node, &pixels.x, &pixels.y);
        pixels.width = texture->width;
        pixels.height = texture->height;
        pixels.values.resize(std::size_t(pixels.width) * pixels.height);
        const wlr_texture_read_pixels_options options{
            .data = pixels.values.data(),
            .format = DRM_FORMAT_ABGR8888,
            .stride = static_cast<std::uint32_t>(pixels.width * 4),
            .dst_x = 0,
            .dst_y = 0,
            .src_box = {0, 0, pixels.width, pixels.height}};
        const bool read = wlr_texture_read_pixels(texture, &options);
        wlr_texture_destroy(texture);
        Require(read, "Real GLES effect pixel readback failed");
        return pixels;
    }

    wl_display *server{}, *client{};
    wlr_backend *backend{};
    wlr_renderer *renderer{};
    wlr_allocator *allocator{};
    wlr_compositor *compositor{};
    wlr_scene *scene{};
    wlr_scene_rect *lower_top{}, *lower_bottom{};
    SurfaceListener surface_listener;
    wlr_surface *server_surface{};
    wlr_xdg_surface base{};
    wlr_xdg_toplevel toplevel{};
    wm::WlrXdgView view;
    std::unique_ptr<wm::SurfaceEffects> effects;
    wl_registry *registry{};
    wl_compositor *client_compositor{};
    wl_shm *client_shm{};
    prism_surface_effect_manager_v1 *manager{};
    prism_surface_effect_v1 *effect{};
    wl_surface *surface{};
    std::uint32_t requested_version{}, advertised_version{};
    unsigned backdrop_events{}, contour_events{}, popup_backdrop_events{};
    bool backdrop_supported{}, contour_supported{}, popup_backdrop_supported{};
    int render_fd{-1};
    std::vector<wlr_surface *> created_surfaces;
    std::vector<std::unique_ptr<ContentListener>> content_listeners;
    std::vector<std::unique_ptr<SharedBuffer>> shared_buffers;

private:
    SharedBuffer *CreateBuffer(int width, int height, std::uint32_t color)
    {
        Require(client_shm, "Missing SHM client global");
        auto buffer = std::make_unique<SharedBuffer>();
        buffer->size = std::size_t(width) * height * 4;
        const int fd = memfd_create("prism-effects-target-test", MFD_CLOEXEC);
        Require(fd >= 0, "Cannot create target SHM storage");
        if (ftruncate(fd, off_t(buffer->size)) < 0) {
            close(fd);
            throw std::runtime_error("Cannot size target SHM storage");
        }
        buffer->pixels = mmap(nullptr, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        auto *pool = wl_shm_create_pool(client_shm, fd, int(buffer->size));
        close(fd);
        Require(buffer->pixels != MAP_FAILED && pool, "Cannot map target SHM storage");
        buffer->native =
            wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        Require(buffer->native, "Cannot create target SHM buffer");
        std::fill_n(static_cast<std::uint32_t *>(buffer->pixels), std::size_t(width) * height,
                    color);
        auto *result = buffer.get();
        shared_buffers.push_back(std::move(buffer));
        return result;
    }

    static void Done(void *data, wl_callback *callback, std::uint32_t)
    {
        static_cast<Completion *>(data)->done = true;
        wl_callback_destroy(callback);
    }

    static void Global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
                       std::uint32_t version)
    {
        auto &self = *static_cast<Fixture *>(data);
        if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
            self.client_compositor = static_cast<wl_compositor *>(
                wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 5u)));
        } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
            self.client_shm =
                static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        } else if (std::strcmp(interface, prism_surface_effect_manager_v1_interface.name) == 0) {
            self.advertised_version = version;
            self.manager = static_cast<prism_surface_effect_manager_v1 *>(
                wl_registry_bind(registry, name, &prism_surface_effect_manager_v1_interface,
                                 std::min(version, self.requested_version)));
            static const prism_surface_effect_manager_v1_listener events{
                Capabilities, ContourCapabilities, PopupBackdropCapabilities};
            prism_surface_effect_manager_v1_add_listener(self.manager, &events, &self);
        }
    }

    static void GlobalRemove(void *, wl_registry *, std::uint32_t)
    {
    }

    static void Capabilities(void *data, prism_surface_effect_manager_v1 *, std::uint32_t value)
    {
        auto &self = *static_cast<Fixture *>(data);
        ++self.backdrop_events;
        self.backdrop_supported = value != 0;
    }

    static void ContourCapabilities(void *data, prism_surface_effect_manager_v1 *,
                                    std::uint32_t value)
    {
        auto &self = *static_cast<Fixture *>(data);
        ++self.contour_events;
        self.contour_supported = value != 0;
    }

    static void PopupBackdropCapabilities(void *data, prism_surface_effect_manager_v1 *,
                                          std::uint32_t value)
    {
        auto &self = *static_cast<Fixture *>(data);
        ++self.popup_backdrop_events;
        self.popup_backdrop_supported = value != 0;
    }

    static void NewSurface(wl_listener *listener, void *data)
    {
        auto *self = reinterpret_cast<SurfaceListener *>(listener);
        auto &fixture = *self->owner;
        auto *surface = static_cast<wlr_surface *>(data);
        if (!fixture.server_surface) {
            fixture.server_surface = surface;
        }
        fixture.created_surfaces.push_back(surface);

        auto content = std::make_unique<ContentListener>();
        content->owner = &fixture;
        content->surface = surface;
        content->commit.notify = ContentCommitted;
        content->destroy.notify = ContentGone;
        wl_list_init(&content->commit.link);
        wl_list_init(&content->destroy.link);
        wl_signal_add(&surface->events.commit, &content->commit);
        wl_signal_add(&surface->events.destroy, &content->destroy);
        fixture.content_listeners.push_back(std::move(content));
    }

    static void ContentCommitted(wl_listener *listener, void *)
    {
        auto *content = reinterpret_cast<ContentListener *>(reinterpret_cast<char *>(listener) -
                                                            offsetof(ContentListener, commit));
        content->owner->effects->NotifySurfaceCommit(content->surface);
    }

    static void ContentGone(wl_listener *listener, void *)
    {
        auto *content = reinterpret_cast<ContentListener *>(reinterpret_cast<char *>(listener) -
                                                            offsetof(ContentListener, destroy));
        content->owner->effects->ForgetSurface(content->surface);
        content->surface = nullptr;
        wl_list_remove(&content->commit.link);
        wl_list_init(&content->commit.link);
        wl_list_remove(&content->destroy.link);
        wl_list_init(&content->destroy.link);
    }
};

void VersionOneAndAtomicState(bool gpu)
{
    Fixture fixture(gpu, 1);
    Require(fixture.advertised_version == 3 && fixture.backdrop_events == 1 &&
                fixture.contour_events == 0 && fixture.popup_backdrop_events == 0 &&
                fixture.backdrop_supported == gpu,
            "Version 1 received incompatible capability events");
    Require(prism_surface_effect_v1_get_version(fixture.effect) == 1,
            "Effect object did not inherit negotiated version 1");

    fixture.AddRectangle();
    Require(fixture.Sync() && !fixture.effects->NeedsUpdate(),
            "Pending region became visible before surface commit");
    Require(!fixture.Update().scene_changed, "Pending request changed scene pixels");
    fixture.Commit();
    Require(fixture.effects->NeedsUpdate(), "Committed effect did not invalidate scene");
    const auto updated = fixture.Update();
    Require(updated.scene_changed == gpu, "Effect publication did not match capability");
    if (gpu) {
        const auto pixels = fixture.ReadEffect();
        Require(pixels.At(29, 10)[0] > 200 && pixels.At(29, 35)[2] > 200 &&
                    pixels.At(29, 10)[1] < 8,
                "Backdrop sampling did not preserve top/bottom or excluded owner boundary");
    }

    prism_surface_effect_v1_clear(fixture.effect);
    Require(fixture.Sync() && !fixture.effects->NeedsUpdate(),
            "Pending clear became visible before commit");
    Require(!fixture.Update().scene_changed, "Pending clear removed current effect");
    fixture.Commit();
    fixture.Update();
    Require(fixture.effects->PresentationNodes(fixture.server_surface).empty(),
            "Committed clear kept stale effect nodes");

    prism_surface_effect_v1_destroy(fixture.effect);
    fixture.effect =
        prism_surface_effect_manager_v1_get_surface_effect(fixture.manager, fixture.surface);
    fixture.AddRectangle();
    Require(fixture.Sync(), "Effect object recreation failed");
    fixture.Commit();
    fixture.Update();
    Require(fixture.effects->PresentationNodes(fixture.server_surface).size() == (gpu ? 1u : 0u),
            "Recreated effect did not publish independent committed state");
}

void GoneSurface(bool gpu, unsigned request)
{
    Fixture fixture(gpu);
    fixture.AddRectangle();
    Require(fixture.Sync(), "Surface-gone setup failed");
    fixture.Commit();
    fixture.Update();
    auto *surface_identity = fixture.server_surface;
    wl_surface_destroy(fixture.surface);
    fixture.surface = nullptr;
    fixture.view.mapped = false;
    Require(fixture.Sync(), "Surface destruction failed");
    Require(fixture.effects->PresentationNodes(surface_identity).empty(),
            "Destroyed owner retained stale effect nodes");
    if (request == 0) {
        prism_surface_effect_v1_clear(fixture.effect);
    } else if (request == 1) {
        fixture.AddRectangle();
    } else {
        fixture.AddContour(Neck());
    }
    fixture.ExpectError(PRISM_SURFACE_EFFECT_V1_ERROR_SURFACE_GONE);
}

void UnsupportedContour()
{
    Fixture fixture(false);
    Require(fixture.backdrop_events == 1 && fixture.contour_events == 1 &&
                fixture.popup_backdrop_events == 0 && !fixture.backdrop_supported &&
                !fixture.contour_supported,
            "Unsupported backend did not publish both false capabilities");
    fixture.AddContour(Neck());
    fixture.ExpectError(PRISM_SURFACE_EFFECT_V1_ERROR_INVALID_REGION);
}

void VersionThreeCapabilities(bool gpu)
{
    Fixture fixture(gpu, 3);
    Require(fixture.advertised_version == 3 && fixture.backdrop_events == 1 &&
                fixture.contour_events == 1 && fixture.popup_backdrop_events == 1 &&
                fixture.backdrop_supported == gpu && fixture.contour_supported == gpu &&
                fixture.popup_backdrop_supported == gpu &&
                prism_surface_effect_v1_get_version(fixture.effect) == 3,
            "Version 3 capabilities did not match the negotiated backend");
}

void BadPayload(std::size_t mutation)
{
    Fixture fixture(true);
    auto payload = contracts::EncodeContour(Neck());
    switch (mutation) {
    case 0:
        payload[0] = 2;
        break;
    case 1:
        payload[4] = 1;
        break;
    case 2:
        payload[6] = 1;
        break;
    case 3:
        payload.pop_back();
        break;
    case 4:
        payload.push_back(0);
        break;
    case 5:
        std::copy_n(payload.begin() + 8, 8, payload.begin() + 16);
        break;
    default:
        throw std::runtime_error("Unknown payload mutation");
    }
    fixture.AddPayload(payload);
    fixture.ExpectError(PRISM_SURFACE_EFFECT_V1_ERROR_INVALID_REGION);
}

void MixedBudget()
{
    Fixture fixture(true);
    for (int i = 0; i < 7; ++i) {
        fixture.AddRectangle(60 + i * 3, 60, 2, 2, 0);
    }
    fixture.AddContour(Neck());
    Require(fixture.Sync(), "Eight mixed regions must be accepted");
    fixture.Commit();
    fixture.Update();
    Require(fixture.effects->PresentationNodes(fixture.server_surface).size() == 8,
            "Eight mixed committed regions did not publish together");
    fixture.AddContour(Neck());
    fixture.ExpectError(PRISM_SURFACE_EFFECT_V1_ERROR_INVALID_REGION);
}

void ContourPixelsAndCache()
{
    Fixture fixture(true);
    Require(fixture.backdrop_events == 1 && fixture.contour_events == 1 &&
                fixture.popup_backdrop_events == 0 && fixture.backdrop_supported &&
                fixture.contour_supported,
            "GPU contour capability was not negotiated");
    Require(prism_surface_effect_v1_get_version(fixture.effect) == 2,
            "Effect object did not inherit version 2");
    fixture.AddContour(Neck());
    Require(fixture.Sync() && !fixture.effects->NeedsUpdate(),
            "Contour request mutated current state before commit");
    fixture.Commit();
    fixture.Update();
    auto initial = fixture.ReadEffect();
    PrintPixel("contour neck", initial, 29, 10);
    PrintPixel("contour upper notch", initial, 15, 10);
    PrintPixel("contour lower body", initial, 15, 35);
    PrintPixel("contour fractional edge", initial, 10, 30);
    Require(initial.At(29, 10)[3] > 245 && initial.At(15, 10)[3] == 0 &&
                initial.At(15, 35)[3] > 245,
            "Asymmetric upper neck/concavity was replaced, flipped or filled as bounds");
    Require(initial.At(29, 10)[0] > 200 && initial.At(29, 35)[2] > 200 && initial.At(29, 10)[1] < 8,
            "Contour backdrop sampled owner pixels or reversed scene orientation");
    Require(initial.At(10, 30)[3] >= 170 && initial.At(10, 30)[3] <= 215,
            "Fractional left origin was lost in coverage projection");

    auto previous = fixture.effects->Counters();
    fixture.effects->MarkDirty();
    Require(!fixture.Update().scene_changed &&
                fixture.effects->Counters().cache_hits > previous.cache_hits,
            "Unchanged contour did not reuse effect pixels");
    Require(fixture.effects->Counters().mask_builds == previous.mask_builds,
            "Unchanged contour rebuilt coverage mask");

    const float yellow[4]{1, 1, 0, 1};
    wlr_scene_rect_set_color(fixture.lower_top, yellow);
    fixture.effects->MarkDirty();
    fixture.Update();
    auto recolored = fixture.ReadEffect();
    Require(recolored.At(29, 10)[0] > 200 && recolored.At(29, 10)[1] > 200 &&
                recolored.At(15, 10)[3] == 0,
            "Background mutation did not repair color while preserving contour");
    Require(fixture.effects->Counters().mask_builds == previous.mask_builds &&
                fixture.effects->Counters().mask_cache_hits > previous.mask_cache_hits,
            "Background-only mutation rebuilt rather than reused coverage");

    auto changed = Neck();
    changed.points[0].x = changed.points[7].x = 28.25;
    changed.points[1].x = changed.points[2].x = 38.25;
    previous = fixture.effects->Counters();
    prism_surface_effect_v1_clear(fixture.effect);
    fixture.AddContour(changed);
    Require(fixture.Sync(), "Same-bounds replacement contour failed");
    Require(!fixture.effects->NeedsUpdate(), "Replacement contour published before commit");
    fixture.Commit();
    fixture.Update();
    auto moved_neck = fixture.ReadEffect();
    Require(moved_neck.At(25, 10)[3] == 0 && moved_neck.At(36, 10)[3] > 245,
            "Same-bounds point changes reused stale contour pixels");
    Require(fixture.effects->Counters().mask_builds > previous.mask_builds &&
                fixture.effects->Counters().rendered_regions > previous.rendered_regions,
            "Same-bounds contour changes did not invalidate both caches");

    prism_surface_effect_v1_clear(fixture.effect);
    fixture.AddContour(Neck(.5));
    Require(fixture.Sync(), "Fractionally shifted contour failed");
    fixture.Commit();
    fixture.Update();
    const auto shifted = fixture.ReadEffect();
    Require(shifted.At(10, 30)[3] >= 45 && shifted.At(10, 30)[3] <= 85,
            "Fractional translation did not preserve subpixel mask origin");

    prism_surface_effect_v1_destroy(fixture.effect);
    fixture.effect =
        prism_surface_effect_manager_v1_get_surface_effect(fixture.manager, fixture.surface);
    fixture.AddContour(Neck());
    Require(fixture.Sync(), "Contour effect recreation failed");
    fixture.Update();
    Require(fixture.ReadEffect().values == shifted.values,
            "Recreating effect object changed current pixels before commit");
    fixture.Commit();
    fixture.Update();
    const auto recreated = fixture.ReadEffect();
    Require(recreated.At(10, 30)[3] >= 170 && recreated.At(10, 30)[3] <= 215,
            "Recreated effect failed to publish new contour state");

    prism_surface_effect_v1_clear(fixture.effect);
    Require(fixture.Sync(), "Pending contour clear failed");
    fixture.effects->MarkDirty();
    fixture.Update();
    Require(fixture.ReadEffect().values == recreated.values,
            "Uncommitted clear removed or changed published contour");
    fixture.Commit();
    fixture.Update();
    Require(fixture.effects->PresentationNodes(fixture.server_surface).empty(),
            "Committed contour clear did not retire GPU effect");
}

void PresentationAndDecoration()
{
    Fixture fixture(true);
    fixture.AddContour(Neck());
    Require(fixture.Sync(), "Presentation contour failed");
    fixture.Commit();
    fixture.view.presentation = wm::NativePresentation{.bounds = {10.75, 12.25, 256, 128},
                                                       .source = {0, 0, 128, 128},
                                                       .decoration = {.enabled = false}};
    fixture.Update();
    const auto transformed = fixture.ReadEffect();
    Require(transformed.At(68, 22)[3] > 245 && transformed.At(40, 22)[3] == 0 &&
                transformed.At(40, 50)[3] > 245,
            "Presentation did not map every neck vertex using nonuniform scale");
    Require(transformed.At(31, 50)[3] >= 170 && transformed.At(31, 50)[3] <= 215,
            "Fractional view origin was lost during transformed mask projection");

    fixture.view.presentation.reset();
    fixture.view.shell_role = 0;
    contracts::ThemeSnapshot theme;
    theme.focused = {.enabled = true,
                     .radius = 12,
                     .border_width = 1,
                     .border = {255, 255, 255, 255},
                     .shadow_blur = 6,
                     .shadow_y = 4,
                     .shadow = {0, 0, 0, 180}};
    const contracts::Contour whole_bounds{
        {{0, 0}, {128, 0}, {128, 128}, {96, 128}, {96, 32}, {0, 32}}};
    prism_surface_effect_v1_clear(fixture.effect);
    fixture.AddContour(whole_bounds);
    Require(fixture.Sync(), "Full bounds concave contour failed");
    fixture.Commit();
    fixture.Update(&theme);
    Require(fixture.effects->PresentationNodes(fixture.server_surface).size() == 2,
            "Full bounds contour was incorrectly reused as window decoration");
    const auto decoration = fixture.ReadEffect(0);
    Require(decoration.At(64, 132)[3] > 100 && decoration.At(64, -4)[3] < 20,
            "Positive decoration shadow_y did not move the shadow downwards");
    const auto contour = fixture.ReadEffect(1);
    Require(contour.At(10, 50)[3] == 0 && contour.At(110, 50)[3] > 245,
            "Decoration pass overwrote requested contour geometry");
}

void MaskBudgetFailureAndRecovery()
{
    Fixture fixture(true);
    const contracts::Contour oversized{{{0, 0}, {4096, 0}, {4096, 4096}, {0, 4096}}};
    const auto before = fixture.effects->Counters();
    fixture.AddContour(oversized, 48);
    Require(fixture.Sync(), "Valid contour transport was rejected before mask budget checking");
    fixture.Commit();
    fixture.Update();
    const auto failed = fixture.effects->Counters();
    Require(failed.mask_failures > before.mask_failures &&
                failed.allocation_attempts == before.allocation_attempts &&
                failed.capture_pass_attempts == before.capture_pass_attempts,
            "Oversized mask did not fail before GPU buffer allocation and backdrop capture");
    Require(fixture.effects->PresentationNodes(fixture.server_surface).empty(),
            "Oversized mask published an invalid GPU effect");
    wlr_scene_node *node;
    wl_list_for_each(node, &fixture.scene->tree.children, link)
    {
        if (node != &fixture.lower_top->node && node != &fixture.lower_bottom->node &&
            node != &fixture.view.scene_tree->node) {
            Require(!node->enabled, "Failed mask left an enabled effect node in the scene");
        }
    }

    prism_surface_effect_v1_clear(fixture.effect);
    fixture.AddContour(Neck());
    Require(fixture.Sync(), "Normal contour could not replace failed mask request");
    fixture.Commit();
    fixture.Update();
    Require(fixture.effects->PresentationNodes(fixture.server_surface).size() == 1 &&
                fixture.effects->Counters().mask_builds > failed.mask_builds &&
                fixture.effects->Counters().rendered_regions > failed.rendered_regions,
            "Normal contour did not recover GPU effect after mask budget failure");
    const auto pixels = fixture.ReadEffect();
    Require(pixels.At(29, 10)[3] > 245 && pixels.At(15, 10)[3] == 0 && pixels.At(15, 35)[3] > 245,
            "Recovered mask did not present the requested neck geometry");
}

void NativeTargetPixelsAndDependencies()
{
    Fixture fixture(true, 3);
    Require(fixture.advertised_version == 3 && fixture.backdrop_events == 1 &&
                fixture.contour_events == 1 && fixture.popup_backdrop_events == 1 &&
                fixture.backdrop_supported && fixture.contour_supported &&
                fixture.popup_backdrop_supported &&
                prism_surface_effect_v1_get_version(fixture.effect) == 3,
            "Native GPU target did not negotiate all version 3 capabilities");
    wlr_scene_node_set_enabled(&fixture.lower_top->node, false);
    wlr_scene_node_set_enabled(&fixture.lower_bottom->node, false);
    wlr_scene_node *legacy;
    wl_list_for_each(legacy, &fixture.view.scene_tree->children, link)
    {
        wlr_scene_node_set_enabled(legacy, false);
    }
    const float green[4]{0, 1, 0, 1};
    auto *backdrop = wlr_scene_rect_create(&fixture.scene->tree, 512, 512, green);
    Require(backdrop, "Cannot create lower native backdrop");
    wlr_scene_node_lower_to_bottom(&backdrop->node);
    wlr_scene_node_set_position(&fixture.view.scene_tree->node, 100, 60);
    fixture.view.x = 100;
    fixture.view.y = 60;

    auto parent = fixture.CreateLayer(fixture.view.scene_tree, 0, 0, 128, 128, 0);
    fixture.Repaint(parent, 0, 0, 64, 64, 0xffff0000u);
    fixture.Repaint(parent, 0, 64, 64, 64, 0xff0000ffu);
    auto popup = fixture.CreateLayer(fixture.view.scene_tree, 20, 12, 128, 128, 0xffffff00u, 6, 8);
    const float purple[4]{1, 0, 1, 1};
    auto *upper = wlr_scene_rect_create(fixture.view.scene_tree, 256, 256, purple);
    Require(upper, "Cannot create excluded upper content");
    const contracts::Contour contour{
        {{38, 8}, {62, 8}, {62, 20}, {102, 20}, {102, 104}, {6, 104}, {6, 20}, {38, 20}}};
    fixture.AddLayerContour(popup, contour);
    const std::array targets{wm::SurfaceEffects::Target{popup.surface, popup.tree}};
    Require(fixture.Update(nullptr, targets).scene_changed, "Native effect did not publish");

    const auto initial = fixture.ReadEffect(0, popup.surface);
    std::printf("native effect global origin=(%d,%d) size=%dx%d\n", initial.x, initial.y,
                initial.width, initial.height);
    Require(initial.x == 116 && initial.y == 68 && initial.width == 104 && initial.height == 104,
            "Native effect lost parent-local position or surface/window geometry compensation");
    Require(initial.At(144, 100)[0] > 200 && initial.At(144, 100)[1] < 8 &&
                initial.At(144, 152)[2] > 200 && initial.At(144, 152)[0] < 8 &&
                initial.At(200, 110)[1] > 200 && initial.At(200, 110)[0] < 8 &&
                initial.At(200, 110)[2] < 8,
            "Native backdrop omitted parent pixels/behind content or sampled itself/upper layer");
    Require(initial.At(160, 76)[3] > 245 && initial.At(124, 76)[3] == 0 &&
                initial.At(118, 100)[3] == 0,
            "Native contour lost its neck, filled a notch or extended into shadow padding");
    const auto paints = fixture.effects->PresentationNodes(popup.surface);
    Require(paints.size() == 1 && paints.front()->parent == popup.tree->node.parent &&
                paints.front()->link.next == &popup.tree->node.link,
            "Native effect was not anchored immediately below its own popup tree");

    auto previous = fixture.effects->Counters();
    fixture.effects->MarkDirty();
    Require(!fixture.Update(nullptr, targets).scene_changed &&
                fixture.effects->Counters().cache_hits > previous.cache_hits &&
                fixture.effects->Counters().capture_passes == previous.capture_passes,
            "Native effect recursively sampled its own paint instead of reusing cache");

    previous = fixture.effects->Counters();
    wl_surface_commit(parent.client);
    Require(fixture.Sync(), "Native parent metadata commit failed");
    Require(fixture.effects->Counters().metadata_commits > previous.metadata_commits &&
                !fixture.effects->NeedsUpdate(),
            "Parent metadata-only commit became a backdrop content revision");
    fixture.effects->MarkDirty();
    Require(!fixture.Update(nullptr, targets).scene_changed &&
                fixture.effects->Counters().capture_passes == previous.capture_passes,
            "Native parent metadata-only commit forced capture");

    previous = fixture.effects->Counters();
    fixture.Repaint(parent, 1, 1, 2, 2, 0xffffff00u);
    Require(!fixture.Update(nullptr, targets).scene_changed &&
                fixture.effects->Counters().partial_damage_cache_hits >
                    previous.partial_damage_cache_hits &&
                fixture.effects->Counters().capture_passes == previous.capture_passes,
            "Outside parent damage invalidated native capture footprint");

    previous = fixture.effects->Counters();
    fixture.Repaint(parent, 24, 28, 32, 32, 0xffffff00u);
    Require(fixture.Update(nullptr, targets).scene_changed &&
                fixture.effects->Counters().capture_passes > previous.capture_passes,
            "Intersecting parent damage did not invalidate native backdrop");
    const auto repaired = fixture.ReadEffect(0, popup.surface);
    Require(repaired.At(144, 100)[0] > 200 && repaired.At(144, 100)[1] > 180 &&
                repaired.At(144, 100)[2] < 8 && repaired.At(124, 76)[3] == 0,
            "Parent damage repair changed geometry or did not sample committed pixels");

    wl_surface_set_buffer_transform(parent.client, WL_OUTPUT_TRANSFORM_180);
    wl_surface_commit(parent.client);
    Require(fixture.Sync(), "Native source transform did not commit");
    fixture.Update(nullptr, targets);
    const auto transformed = fixture.ReadEffect(0, popup.surface);
    Require(transformed.At(144, 100)[1] > 200 && transformed.At(144, 100)[0] < 8 &&
                transformed.At(200, 110)[2] > 200 && transformed.At(200, 110)[0] < 8,
            "Native capture ignored source buffer transform or reused stale mapping epoch");
    wl_surface_set_buffer_transform(parent.client, WL_OUTPUT_TRANSFORM_NORMAL);
    wl_surface_commit(parent.client);
    Require(fixture.Sync(), "Native source transform did not restore");
    fixture.Update(nullptr, targets);
    Require(fixture.ReadEffect(0, popup.surface).values == repaired.values,
            "Native buffer transform restoration lost original logical content");

    previous = fixture.effects->Counters();
    wlr_scene_node_set_position(&fixture.view.scene_tree->node, 200, 100);
    fixture.view.x = 200;
    fixture.view.y = 100;
    fixture.effects->MarkDirty();
    fixture.Update(nullptr, targets);
    const auto moved = fixture.ReadEffect(0, popup.surface);
    Require(moved.x == repaired.x + 100 && moved.y == repaired.y + 40 &&
                moved.values == repaired.values &&
                fixture.effects->Counters().mask_builds == previous.mask_builds,
            "Moving a nonzero native parent double-translated effect or rebuilt its local mask");

    auto nested = fixture.CreateLayer(popup.tree, 40, 32, 48, 48, 0xffaa00ffu, 3, 5);
    const contracts::Contour nested_contour{{{3, 5}, {35, 5}, {35, 37}, {3, 37}}};
    fixture.AddLayerContour(nested, nested_contour);
    // Supply in reverse order; real scene order determines dependency resolution.
    const std::array nested_targets{wm::SurfaceEffects::Target{nested.surface, nested.tree},
                                    wm::SurfaceEffects::Target{popup.surface, popup.tree}};
    fixture.Update(nullptr, nested_targets);
    const auto child = fixture.ReadEffect(0, nested.surface);
    Require(child.x == 256 && child.y == 140 && child.width == 40 && child.height == 40,
            "Nested native effect double-applied parent origin or explicit surface inset");
    Require(child.At(275, 159)[0] > 200 && child.At(275, 159)[1] > 200 && child.At(275, 159)[2] < 8,
            "Nested native capture excluded its parent or included itself/upper siblings");
    previous = fixture.effects->Counters();
    fixture.effects->MarkDirty();
    Require(!fixture.Update(nullptr, nested_targets).scene_changed &&
                fixture.effects->Counters().capture_passes == previous.capture_passes,
            "Nested paints did not resolve and cache in actual lower-to-upper order");

    wlr_scene_node_destroy(&fixture.view.scene_tree->node);
    fixture.view.scene_tree = nullptr;
    fixture.view.mapped = false;
    Require(fixture.effects->PresentationNodes(popup.surface).empty() &&
                fixture.effects->PresentationNodes(nested.surface).empty(),
            "Parent tree destruction left native paint pointers advertised as valid");
    fixture.effects->MarkDirty();
    Require(fixture.Update().scene_changed,
            "Unregistered targets did not retire paints after parent tree destruction");
    wl_surface_destroy(popup.client);
    wl_surface_destroy(nested.client);
    Require(fixture.Sync(), "Surviving native surfaces failed teardown after parent tree gone");
}
} // namespace

int main(int argc, char **argv)
{
    try {
        const bool gpu = argc == 2 && std::string_view(argv[1]) == "--gpu";
        Require(argc == 1 || gpu, "Usage: surface_effects_protocol_test [--gpu]");
        VersionOneAndAtomicState(gpu);
        GoneSurface(gpu, 0);
        GoneSurface(gpu, 1);
        GoneSurface(gpu, 2);
        if (gpu) {
            for (std::size_t mutation = 0; mutation < 6; ++mutation) {
                BadPayload(mutation);
            }
            MixedBudget();
            ContourPixelsAndCache();
            PresentationAndDecoration();
            MaskBudgetFailureAndRecovery();
            NativeTargetPixelsAndDependencies();
        } else {
            UnsupportedContour();
            VersionThreeCapabilities(false);
        }
        std::printf("surface effect protocol and %s integration passed\n", gpu ? "GPU" : "pixman");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "surface effect protocol: %s\n", error.what());
        return 1;
    }
}
