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
#include <vector>

namespace prism::sdk {
namespace {
void AddSceneStats(ClientRenderStats& total,const runtime::SceneRenderStats& scene) {
    total.scene_build_attempts += scene.build_calls;
    total.scene_builds += scene.builds;
    total.scene_layouts += scene.layouts;
}
}
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
    bool PollResources();
    void CloseGpu();
    void FailFrontend();
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
    ClientRenderStats render_stats{}; // Scene fields retain retired UI counters.
    std::uint64_t committed_pixels_revision{0};
    std::uint64_t prepared_pixels_revision{0};
    bool state_prepared{false};
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
        if (app.scene) AddSceneStats(app.render_stats,app.scene->GetRenderStats());
        app.scene = std::move(next);
        app.scene_images = std::move(images);
        app.last_list.reset();
        app.committed_pixels_revision = app.prepared_pixels_revision = 0;
        app.state_prepared = false;
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
    impl_->window.RequestUpdate(true);
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
            if(app.scene->SetPointer(motion->position)) app.window.RequestUpdate(true);
        } else if (auto* key = std::get_if<contracts::KeyEvent>(&event)) {
            if(key->state==contracts::ButtonState::Pressed) {
                if(key->physical_key==0x2B && app.scene->FocusNext()) app.window.RequestUpdate(true);
                else if((key->physical_key==0x28 || key->physical_key==0x2C) && app.on_action)
                    if(auto action=app.scene->FocusedAction()) app.on_action(*action);
            }
        } else if (auto* button = std::get_if<contracts::PointerButtonEvent>(&event)) {
            if (button->state == contracts::ButtonState::Pressed && button->button==contracts::PointerButton::Primary && app.on_action) {
                if (auto action = app.scene->ActionAt(button->position)) app.on_action(*action);
            }
        }
    });
    app.window.SetSubmitHandlers([&app](const platform::SubmitRequest& request) {
        app.state_prepared = false;
        if (app.failed || !app.scene) return platform::SubmitResult::Failed;
        const auto dirty = app.scene->PendingDirty();
        const bool pixels = request.force_pixels || !app.last_list ||
            app.scene->PixelsRevision() != app.committed_pixels_revision;
        // Keep geometry/material metadata with the corresponding new pixels.
        // A pure Composite change is allowed through an old pixel callback.
        if (pixels && !request.allow_pixels) return platform::SubmitResult::None;
        try {
            if (pixels) {
                if (!app.egl.Ready()) {
                    if (!app.egl.Open(request.display, request.surface, request.width, request.height))
                        throw std::runtime_error("EGL surface initialization failed");
                    app.gl_renderer = app.egl.GlRenderer();
                    render_skia::GlesRendererOptions options;
                    if (app.config.gpu_resource_cache_bytes)
                        options.resource_cache_bytes = *app.config.gpu_resource_cache_bytes;
                    app.renderer = std::make_unique<render_skia::GlesRenderer>(app.commands, options);
                }
                if (!app.renderer || !app.renderer->Ready() ||
                    !app.egl.Resize(request.width, request.height) || !app.egl.MakeCurrent())
                    throw std::runtime_error("EGL renderer unavailable");
                if (!app.last_list || runtime::Has(dirty, runtime::Dirty::Layout) ||
                    runtime::Has(dirty, runtime::Dirty::Paint)) {
                    if (auto next = app.scene->Build(contracts::WindowId{1}))
                        app.last_list = std::move(next);
                }
                if (!app.last_list) throw std::runtime_error("Scene has no pixel display list");
            } else if (!runtime::Has(dirty, runtime::Dirty::Composite)) {
                return platform::SubmitResult::None;
            }
            app.window.SetSurfaceEffects(app.scene->SurfaceEffects());
            app.window.SetInputRegions(app.scene->InputRegions());
            app.state_prepared = true;
            if (!pixels) return app.window.SurfaceStatePending()
                ? platform::SubmitResult::State : platform::SubmitResult::None;
            ++app.render_stats.gpu_render_attempts;
            if (!app.renderer->Render(*app.last_list, request.width, request.height))
                throw std::runtime_error("GPU rendering failed");
            ++app.render_stats.gpu_render_successes;
            app.prepared_pixels_revision = app.scene->PixelsRevision();
            return platform::SubmitResult::Pixels;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[prism-sdk] submission failed: %s\n", error.what());
            app.FailFrontend();
            return platform::SubmitResult::Failed;
        }
    }, [&app] {
        ++app.render_stats.swap_attempts;
        if (!app.egl.Swap()) { app.FailFrontend(); return false; }
        ++app.render_stats.swap_successes;
        ++app.presented;
        return true;
    }, [&app](platform::SubmitResult result) {
        if (result == platform::SubmitResult::Failed) {
            app.FailFrontend();
            return;
        }
        if (result == platform::SubmitResult::Pixels)
            app.committed_pixels_revision = app.prepared_pixels_revision;
        // None can acknowledge a checked, identical metadata request, e.g.
        // when the optional effects extension is unavailable.
        if (app.state_prepared && app.scene) app.scene->AcknowledgeComposite();
        app.state_prepared = false;
    });
    if (!app.window.Open(app.config.socket, app.config.app_id, app.config.title,
                         app.config.width, app.config.height)) {
        if (app.scene) AddSceneStats(app.render_stats,app.scene->GetRenderStats());
        app.scene.reset();
        return false;
    }
    app.opened_once = true;
    return true;
}

void ClientApplication::Impl::CloseGpu() {
    // EGL owns native objects backed by the Wayland surface. Release them
    // before the platform's terminal failure destroys that surface/display.
    // Another ClientApplication on this thread may have made its GL context
    // current. Ganesh must delete resources in this renderer's own context.
    if (renderer) {
        if (!egl.MakeCurrent()) renderer->Abandon();
        renderer.reset();
    }
    egl.Close();
}

void ClientApplication::Impl::FailFrontend() {
    failed = true;
    CloseGpu();
}

bool ClientApplication::Impl::PollResources() {
    const auto updates = resources.Poll();
    for (const auto& update : updates) {
        if (!scene_images.contains(update.id.value)) continue;
        const auto* image = resources.Get(update.id);
        if (update.state != runtime::ImageState::Ready || !image ||
            !commands.RegisterImage(update.id, *image) ||
            !scene->ImageReady(update.id, update.intrinsic_size)) {
            FailFrontend();
            continue;
        }
        ++loaded_images;
        if (scene->PendingDirty() != runtime::Dirty::None) window.RequestUpdate(true);
    }
    return !updates.empty();
}

bool ClientApplication::Pump(int timeout_ms, std::span<pollfd> wake_fds) {
    auto& app = *impl_;
    for (auto& fd : wake_fds) fd.revents = 0;
    if (!app.scene || app.failed) return false;
    try {
        // A consumed resource completion must return control to the host even
        // if it belonged to a hidden subtree and produced no pixel dirtiness.
        if (app.PollResources()) timeout_ms = 0;
        if (app.failed) { app.window.Close(); return false; }
        std::vector<pollfd> sources;
        const int completion_fd = app.resources.CompletionFd();
        const std::size_t resource_sources = completion_fd >= 0 ? 1 : 0;
        sources.reserve(resource_sources + wake_fds.size());
        if (resource_sources) sources.push_back({completion_fd, POLLIN, 0});
        sources.insert(sources.end(), wake_fds.begin(), wake_fds.end());
        const bool running = app.window.Pump(timeout_ms, sources);
        for (std::size_t i = 0; i < wake_fds.size(); ++i)
            wake_fds[i].revents = sources[resource_sources + i].revents;
        if (!running) {
            app.FailFrontend();
            app.window.Close();
            return false;
        }
        app.PollResources();
        if (app.failed) { app.window.Close(); return false; }
        // Resource updates defer submission; the next host turn re-collects
        // its sources, then Window submits before entering the next wait.
        return true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[prism-sdk] event pump failed: %s\n", error.what());
        app.FailFrontend();
        app.window.Close();
        return false;
    }
}
bool ClientApplication::SetSlot(std::string_view name, std::string value) {
    return SetBinding(name, std::move(value));
}
bool ClientApplication::SetBinding(std::string_view name, runtime::PropertyValue value) {
    auto& app = *impl_;
    if (!app.scene || !app.scene->AcceptsBinding(name, value)) return false;
    app.scene->SetBinding(name, std::move(value));
    if (app.scene->PendingDirty() != runtime::Dirty::None) app.window.RequestUpdate(true);
    return true;
}
bool ClientApplication::ApplyTheme(const contracts::ThemeSnapshot& theme, std::string* diagnostic) {
    auto& app = *impl_;
    try {
        contracts::ValidateTheme(theme);
        std::optional<contracts::ThemeSnapshot> prepared(theme);
        if (app.scene && !app.scene->ApplyTheme(theme,diagnostic)) return false;
        app.theme.swap(prepared);
        if (app.scene && app.scene->PendingDirty() != runtime::Dirty::None)
            app.window.RequestUpdate(true);
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
bool ClientApplication::FrameCallbackPending() const { return impl_->window.FrameCallbackPending(); }
int ClientApplication::PresentedCount() const { return impl_->presented; }
bool ClientApplication::HasPresentationFeedback() const { return impl_->window.HasPresentationFeedback(); }
int ClientApplication::PresentationCount() const { return impl_->window.PresentationCount(); }
ClientRenderStats ClientApplication::GetRenderStats() const {
    auto stats=impl_->render_stats;
    if (impl_->scene) AddSceneStats(stats,impl_->scene->GetRenderStats());
    stats.frame_callbacks_done=static_cast<std::uint64_t>(impl_->window.FrameDoneCount());
    const auto submitted = impl_->window.GetSubmitStats();
    stats.surface_state_commits = submitted.state_commits;
    stats.surface_pixel_commits = submitted.pixel_commits;
    stats.surface_submission_failures = submitted.failures;
    stats.surface_noops = submitted.none;
    return stats;
}
int ClientApplication::RequestedImageCount() const { return impl_->requested_images; }
int ClientApplication::LoadedImageCount() const { return impl_->loaded_images; }
std::string ClientApplication::GlRenderer() const { return impl_->gl_renderer; }
void ClientApplication::Close() {
    if (!impl_) return;
    impl_->CloseGpu();
    impl_->window.Close();
    if (impl_->scene) AddSceneStats(impl_->render_stats,impl_->scene->GetRenderStats());
    impl_->scene.reset();
    impl_->last_list.reset();
}
} // namespace prism::sdk
