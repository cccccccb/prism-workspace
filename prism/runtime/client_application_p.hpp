#pragma once
#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/buffer_damage.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/sdk/client_application.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace prism::sdk {
struct ClientApplication::Impl {
    explicit Impl(ClientConfig value)
        : config(std::move(value)), commands(config.font_path),
          resources(render_skia::RasterRenderer::DecodePng)
    {
    }

    bool LoadScene(std::string_view source);
    contracts::ResourceId RequestImage(std::set<std::uint64_t> &images, std::string_view uri);
    runtime::ShapedText ShapeText(std::string_view text, double size);
    void HandleWindowEvent(const contracts::WindowEvent &event);
    platform::SubmitResult PrepareSubmit(const platform::SubmitRequest &request);
    bool CommitPixels();
    void Submitted(platform::SubmitResult result);
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
    std::shared_ptr<const contracts::DisplayList> committed_list, prepared_list;
    runtime::BufferDamageHistory damage_history;
    std::optional<runtime::BufferDamagePlan> prepared_damage;
    std::uint64_t committed_resource_epoch{}, prepared_resource_epoch{};
    std::uint64_t prepared_content_area{};
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

inline void AddSceneStats(ClientRenderStats &total, const runtime::SceneRenderStats &scene)
{
    total.scene_build_attempts += scene.build_calls;
    total.scene_builds += scene.builds;
    total.scene_layouts += scene.layouts;
}
} // namespace prism::sdk
