#include "prism/sdk/client_application.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace prism::sdk {
struct ClientApplication::Impl {
    explicit Impl(ClientConfig value)
        : config(std::move(value)), commands(config.font_path),
          resources(render_skia::RasterRenderer::DecodePng) {}

    ClientConfig config;
    render_skia::RasterRenderer commands;
    runtime::ImageResources resources;
    std::unique_ptr<runtime::Scene> scene;
    std::optional<contracts::DisplayList> last_list;
    platform::WaylandWindow window;
    platform::WaylandEglSurface egl;
    std::unique_ptr<render_skia::GlesRenderer> renderer;
    std::function<void(std::string_view)> on_action;
    std::string gl_renderer;
    int requested_images{0};
    int loaded_images{0};
    int presented{0};
    bool failed{false};
    bool opened_once{false};
};

ClientApplication::ClientApplication(ClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
ClientApplication::~ClientApplication() { Close(); }

bool ClientApplication::Open(std::string_view dsl_source) {
    auto& app = *impl_;
    if (app.opened_once || !app.commands.Ready() || app.config.socket.empty() ||
        app.config.app_id.empty()) return false;
    try {
        app.scene = std::make_unique<runtime::Scene>(runtime::ParseBlueprint(dsl_source,
            [&](std::string_view uri) {
                ++app.requested_images;
                return app.resources.Request(std::string(uri));
            }),
            [&](std::string_view text, double size) { return app.commands.Shape(text, size); },
            app.commands.FontId());
    } catch (const std::exception&) {
        app.scene.reset();
        return false;
    }
    app.window.SetEventHandler([&app](const contracts::WindowEvent& event) {
        if (auto* configure = std::get_if<contracts::ConfigureEvent>(&event)) {
            app.scene->SetViewport(configure->metrics.logical_size);
        } else if (auto* button = std::get_if<contracts::PointerButtonEvent>(&event)) {
            if (button->state == contracts::ButtonState::Pressed && app.on_action) {
                if (auto action = app.scene->ActionAt(button->position)) app.on_action(*action);
            }
        }
    });
    app.window.SetPresentHandler([&app](wl_display* display, wl_surface* surface,
                                         int width, int height) {
        if (!app.egl.Ready()) {
            if (!app.egl.Open(display, surface, width, height)) { app.failed = true; return false; }
            app.gl_renderer = app.egl.GlRenderer();
            app.renderer = std::make_unique<render_skia::GlesRenderer>(app.commands);
        }
        if (!app.renderer || !app.renderer->Ready() || !app.egl.Resize(width, height) ||
            !app.egl.MakeCurrent()) { app.failed = true; return false; }
        if (auto next = app.scene->Build(contracts::WindowId{1})) app.last_list = std::move(next);
        if (!app.last_list || !app.renderer->Render(*app.last_list, width, height) ||
            !app.egl.Swap()) { app.failed = true; return false; }
        ++app.presented;
        return true;
    });
    if (!app.window.Open(app.config.socket, app.config.app_id, app.config.title,
                         app.config.width, app.config.height)) {
        app.scene.reset();
        return false;
    }
    app.opened_once = true;
    return true;
}

bool ClientApplication::Pump(int timeout_ms) {
    auto& app = *impl_;
    if (!app.scene || app.failed || !app.window.Pump(timeout_ms)) return false;
    for (const auto& update : app.resources.Poll()) {
        if (update.state != runtime::ImageState::Ready) { app.failed = true; continue; }
        const auto* image = app.resources.Get(update.id);
        if (!image || !app.commands.RegisterImage(update.id, *image) ||
            !app.scene->ImageReady(update.id, update.intrinsic_size)) {
            app.failed = true;
            continue;
        }
        ++app.loaded_images;
        app.window.RequestRedraw();
    }
    return !app.failed;
}
bool ClientApplication::SetSlot(std::string_view name, std::string value) {
    return SetBinding(name, std::move(value));
}
bool ClientApplication::SetBinding(std::string_view name, runtime::PropertyValue value) {
    auto& app = *impl_;
    if (!app.scene || !app.scene->SetBinding(name, std::move(value))) return false;
    if (app.scene->PendingDirty() != runtime::Dirty::None) app.window.RequestRedraw();
    return true;
}
void ClientApplication::OnAction(std::function<void(std::string_view)> callback) {
    impl_->on_action = std::move(callback);
}
bool ClientApplication::IsCloseRequested() const { return impl_->window.IsCloseRequested(); }
bool ClientApplication::IsMapped() const { return impl_->window.IsMapped(); }
int ClientApplication::ConfigureCount() const { return impl_->window.ConfigureCount(); }
int ClientApplication::FrameDoneCount() const { return impl_->window.FrameDoneCount(); }
int ClientApplication::PresentedCount() const { return impl_->presented; }
int ClientApplication::RequestedImageCount() const { return impl_->requested_images; }
int ClientApplication::LoadedImageCount() const { return impl_->loaded_images; }
std::string ClientApplication::GlRenderer() const { return impl_->gl_renderer; }
void ClientApplication::Close() {
    if (!impl_) return;
    impl_->renderer.reset();
    impl_->egl.Close();
    impl_->window.Close();
    impl_->scene.reset();
    impl_->last_list.reset();
}
} // namespace prism::sdk
