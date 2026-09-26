#include "prism/sdk/client_application.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include <optional>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <variant>
#include <set>
#include <cstdio>

namespace prism::sdk {
std::optional<std::string> LoadUiSource(std::string_view installed_name,
                                        std::string_view source_path) {
    const auto installed_ui = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path()
        / "share/prism/ui" / installed_name;
    for (const auto& path : {std::string(source_path), "../" + std::string(source_path),
                             installed_ui.string()}) {
        std::ifstream input(path);
        if (input) return std::string(std::istreambuf_iterator<char>{input}, {});
    }
    return std::nullopt;
}

struct ClientApplication::Impl {
    explicit Impl(ClientConfig value)
        : config(std::move(value)), commands(config.font_path),
          resources(render_skia::RasterRenderer::DecodePng) {}

    bool LoadScene(std::string_view source);
    std::set<std::uint64_t> scene_images;
    ClientConfig config;
    render_skia::RasterRenderer commands;
    runtime::ImageResources resources;
    std::unique_ptr<runtime::Scene> scene;
    std::optional<contracts::ThemeSnapshot> theme;
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

bool ClientApplication::Impl::LoadScene(std::string_view dsl_source) {
    auto& app = *this;
    std::set<std::uint64_t> images;
    try {
        auto next = std::make_unique<runtime::Scene>(runtime::ParseBlueprint(dsl_source,
            [&](std::string_view uri) {
                ++app.requested_images;
                std::string path(uri);
                if (!app.config.assets_root.empty()) {
                    const std::filesystem::path relative(path);
                    if (relative.is_absolute() || path.find('\\') != std::string::npos)
                        throw std::invalid_argument("Invalid package image URI");
                    for (const auto& part : relative)
                        if (part == "..") throw std::invalid_argument("Image traversal");
                    const auto root = std::filesystem::canonical(app.config.assets_root);
                    const auto resolved = std::filesystem::canonical(root / relative);
                    auto a = root.begin(), b = resolved.begin();
                    for (; a != root.end() && b != resolved.end() && *a == *b; ++a, ++b) {}
                    if (a != root.end() || !std::filesystem::is_regular_file(resolved))
                        throw std::invalid_argument("Image outside package assets");
                    path = resolved.string();
                } else if (!std::filesystem::exists(path)) {
                    const auto development = std::filesystem::path("resources") / path;
                    const auto installed = std::filesystem::canonical("/proc/self/exe")
                        .parent_path().parent_path() / "share/prism" / path;
                    if (std::filesystem::exists(development)) path = development.string();
                    else if (std::filesystem::exists(installed)) path = installed.string();
                }
                auto id = app.resources.Request(std::move(path));
                images.insert(id.value);
                return id;
            }),
            [&](std::string_view text, double size) { return app.commands.Shape(text, size); },
            app.commands.FontId(),app.theme);
        if (app.window.IsConfigured()) next->SetViewport(app.window.Metrics().logical_size);
        for (auto value : images) {
            const contracts::ResourceId id{value};
            if (app.resources.State(id) == runtime::ImageState::Failed)
                throw std::runtime_error("Package image unavailable");
            if (const auto* image = app.resources.Get(id)) {
                if (!app.commands.RegisterImage(id, *image) ||
                    !next->ImageReady(id, {static_cast<double>(image->width), static_cast<double>(image->height)}))
                    throw std::runtime_error("Cached image registration failed");
            }
        }
        app.scene = std::move(next);
        app.scene_images = std::move(images);
        app.last_list.reset();
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool ClientApplication::FrontendReady() const { return impl_->commands.Ready(); }
bool ClientApplication::ConfigureWindow(ClientConfig config) {
    if (impl_->opened_once || config.font_path != impl_->config.font_path) return false;
    impl_->config = std::move(config);
    return true;
}
bool ClientApplication::ReplaceUi(std::string_view source) {
    if (!impl_->opened_once || !impl_->LoadScene(source)) return false;
    impl_->window.RequestRedraw(true);
    return true;
}

bool ClientApplication::Open(std::string_view dsl_source) {
    auto& app = *impl_;
    if (app.opened_once || !app.commands.Ready() ||
        app.config.app_id.empty()) return false;
    if (!app.LoadScene(dsl_source)) return false;
    app.window.SetEventHandler([&app](const contracts::WindowEvent& event) {
        if (auto* configure = std::get_if<contracts::ConfigureEvent>(&event)) {
            app.scene->SetViewport(configure->metrics.logical_size);
        } else if (auto* motion = std::get_if<contracts::PointerMotionEvent>(&event)) {
            if(app.scene->SetPointer(motion->position)) app.window.RequestRedraw(true);
        } else if (auto* key = std::get_if<contracts::KeyEvent>(&event)) {
            if(key->state==contracts::ButtonState::Pressed) {
                if(key->physical_key==0x2B && app.scene->FocusNext()) app.window.RequestRedraw(true);
                else if((key->physical_key==0x28 || key->physical_key==0x2C) && app.on_action)
                    if(auto action=app.scene->FocusedAction()) app.on_action(*action);
            }
        } else if (auto* button = std::get_if<contracts::PointerButtonEvent>(&event)) {
            if (button->state == contracts::ButtonState::Pressed && button->button==contracts::PointerButton::Primary && app.on_action) {
                if (auto action = app.scene->ActionAt(button->position)) app.on_action(*action);
            }
        }
    });
    app.window.SetPresentHandler([&app](wl_display* display, wl_surface* surface,
                                         int width, int height) {
        if (!app.egl.Ready()) {
            if (!app.egl.Open(display, surface, width, height)) { app.failed = true; return false; }
            app.gl_renderer = app.egl.GlRenderer();
            render_skia::GlesRendererOptions options;
            if (app.config.gpu_resource_cache_bytes) options.resource_cache_bytes=*app.config.gpu_resource_cache_bytes;
            app.renderer = std::make_unique<render_skia::GlesRenderer>(app.commands,options);
        }
        if (!app.renderer || !app.renderer->Ready() || !app.egl.Resize(width, height) ||
            !app.egl.MakeCurrent()) { app.failed = true; return false; }
        if (auto next = app.scene->Build(contracts::WindowId{1})) app.last_list = std::move(next);
        try {
            app.window.SetSurfaceEffects(app.scene->SurfaceEffects());
        } catch(const std::exception& error) {
            std::fprintf(stderr,"[prism-sdk] surface effect request rejected: %s\n",error.what());
            app.failed=true;
            return false;
        }
        app.window.SetInputRegions(app.scene->InputRegions());
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
        if (!app.scene_images.contains(update.id.value)) continue;
        if (update.state != runtime::ImageState::Ready) { app.failed = true; continue; }
        const auto* image = app.resources.Get(update.id);
        if (!image || !app.commands.RegisterImage(update.id, *image) ||
            !app.scene->ImageReady(update.id, update.intrinsic_size)) {
            app.failed = true;
            continue;
        }
        ++app.loaded_images;
        app.window.RequestRedraw(true);
    }
    return !app.failed;
}
bool ClientApplication::SetSlot(std::string_view name, std::string value) {
    return SetBinding(name, std::move(value));
}
bool ClientApplication::SetBinding(std::string_view name, runtime::PropertyValue value) {
    auto& app = *impl_;
    if (!app.scene || !app.scene->AcceptsBinding(name, value)) return false;
    app.scene->SetBinding(name, std::move(value));
    if (app.scene->PendingDirty() != runtime::Dirty::None) app.window.RequestRedraw(true);
    return true;
}
bool ClientApplication::ApplyTheme(const contracts::ThemeSnapshot& theme, std::string* diagnostic) {
    auto& app = *impl_;
    try {
        contracts::ValidateTheme(theme);
        std::optional<contracts::ThemeSnapshot> prepared(theme);
        if (app.scene && !app.scene->ApplyTheme(theme,diagnostic)) return false;
        app.theme.swap(prepared);
        if (app.scene) app.window.RequestRedraw(true);
        if (diagnostic) diagnostic->clear();
        return true;
    } catch (const std::exception& error) {
        if (diagnostic) *diagnostic=error.what();
        return false;
    }
}
std::uint64_t ClientApplication::ThemeGeneration() const {
    return impl_->theme ? impl_->theme->generation : 0;
}
void ClientApplication::OnAction(std::function<void(std::string_view)> callback) {
    impl_->on_action = std::move(callback);
}
bool ClientApplication::IsCloseRequested() const { return impl_->window.IsCloseRequested(); }
bool ClientApplication::IsMapped() const { return impl_->window.IsMapped(); }
int ClientApplication::ConfigureCount() const { return impl_->window.ConfigureCount(); }
int ClientApplication::FrameDoneCount() const { return impl_->window.FrameDoneCount(); }
int ClientApplication::PresentedCount() const { return impl_->presented; }
bool ClientApplication::HasPresentationFeedback() const { return impl_->window.HasPresentationFeedback(); }
int ClientApplication::PresentationCount() const { return impl_->window.PresentationCount(); }
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
