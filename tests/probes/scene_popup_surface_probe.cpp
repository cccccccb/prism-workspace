#include "prism/contracts/contour.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include "prism/platform/wayland_egl_context.hpp"
#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/platform/wayland_popup.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/popup_surface.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/theme/compiler.hpp"

#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <wayland-client.h>

// Manual, isolated GPU gate. It consumes actual Scene/DSL export plans; no
// application input or automatic Host child lifecycle is asserted by this probe.
namespace {
using namespace prism;
using namespace std::chrono_literals;
constexpr const char *font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

void Require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

contracts::NodeId FindAction(const runtime::InputSnapshot &snapshot, std::string_view action)
{
    for (const auto &node : snapshot.nodes) {
        if (node.id && node.action == action) {
            return node.id;
        }
    }
    throw std::runtime_error("missing fixture action: " + std::string(action));
}

class Frame {
public:
    Frame(platform::WaylandWindow &window, wl_surface *surface)
        : window_(window), display_(window.Display()), callback_(wl_surface_frame(surface))
    {
        Require(callback_, "frame request failed");
        static const wl_callback_listener listener{Done};
        if (wl_callback_add_listener(callback_, &listener, this) < 0) {
            wl_callback_destroy(callback_);
            callback_ = nullptr;
            throw std::runtime_error("frame listener failed");
        }
    }

    ~Frame()
    {
        if (callback_ && window_.Display() == display_) {
            wl_callback_destroy(callback_);
        }
    }

    void Wait()
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!done_) {
            Require(std::chrono::steady_clock::now() < deadline, "frame timed out");
            Require(window_.Pump(20), "window stopped while awaiting frame");
        }
    }

private:
    static void Done(void *data, wl_callback *callback, std::uint32_t) noexcept
    {
        auto &self = *static_cast<Frame *>(data);
        wl_callback_destroy(callback);
        self.callback_ = nullptr;
        self.done_ = true;
    }

    platform::WaylandWindow &window_;
    wl_display *display_{};
    wl_callback *callback_{};
    bool done_{};
};

class Probe {
public:
    Probe() : shaper_(font_path), raster_(font_path)
    {
        Require(shaper_.Ready() && raster_.Ready(), "font setup failed");
    }

    ~Probe()
    {
        Cleanup();
    }

    void Run(const std::string &socket, const std::filesystem::path &root, bool any_gpu)
    {
        std::ifstream input(root / "tests/fixtures/interface-system/attached-panel.prism");
        Require(input.good(), "fixture could not be read");
        const std::string source{std::istreambuf_iterator<char>(input), {}};
        window_.SetPaintHandler(std::bind_front(&Probe::Bootstrap, this));
        Require(window_.Open(socket, "prism.popup-surface-plan", "Scene export probe", 640, 480),
                "parent failed to open");
        WaitForMapping();
        metrics_ = window_.Metrics();
        window_.SetSubmitHandlers(std::bind_front(&Probe::Idle, this), {});
        window_.SetBeforeCloseHandler(std::bind_front(&Probe::ReleaseGpu, this));
        Require(context_.Open(window_.Display()), "shared context failed");
        Require(parent_.Open(context_, window_.Surface(), int(metrics_.buffer_size.width),
                             int(metrics_.buffer_size.height)),
                "parent WSI failed");
        driver_ = parent_.GlRenderer();
        Require(any_gpu || driver_.find("V3D") != std::string::npos, "GPU is not V3D");
        renderer_ =
            std::make_unique<render_skia::GlesRenderer>(font_path, parent_.TargetIdentity());
        Require(renderer_->Ready(), "Ganesh initialization failed");

        for (const auto *material : {"glass", "translucent", "transparent", "square"}) {
            for (const auto *scheme : {"light", "dark"}) {
                const auto theme = theme::LoadTheme(root / "resources/themes", material, 1, scheme);
                for (unsigned position = 0; position < 3; ++position) {
                    RunCase(source, theme, material, scheme, position);
                }
            }
        }

        const auto stats = renderer_->GetRenderStats();
        Require(cases_ == 24 && child_hooks_ == cases_, "cases or child cleanup missing");
        Require(stats.target_wraps == cases_ + 1 && stats.target_releases == cases_,
                "target wrappers were not retained/released as expected");
        Require(parent_.Ready() && context_.Ready() && renderer_->Ready(),
                "child close invalidated the parent");
        std::cout << "scene_popup_surface_probe: passed renderer=" << driver_ << " cases=" << cases_
                  << " frames=" << frames_ << " sampled_pixels=" << samples_
                  << " wraps=" << stats.target_wraps << " releases=" << stats.target_releases
                  << " child_hooks=" << child_hooks_ << '\n';
        Cleanup();
    }

private:
    void Bootstrap(void *pixels, int width, int height, int stride)
    {
        for (int y = 0; y < height; ++y) {
            auto *row = reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(pixels) +
                                                          std::size_t(y) * stride);
            std::fill_n(row, width, 0xff203040u);
        }
    }

    platform::SubmitResult Idle(const platform::SubmitRequest &)
    {
        return platform::SubmitResult::None;
    }

    void WaitForMapping()
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!window_.IsMapped() || window_.FrameCallbackPending()) {
            Require(std::chrono::steady_clock::now() < deadline, "parent map timed out");
            Require(window_.Pump(20), "parent stopped before map");
        }
    }

    void WaitForPopup()
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!popup_.IsConfigured()) {
            Require(!popup_.IsClosed(), "popup dismissed before configure");
            Require(std::chrono::steady_clock::now() < deadline, "popup configure timed out");
            Require(window_.Pump(20), "parent stopped before popup configure");
        }
    }

    void RunCase(const std::string &source, const contracts::ThemeSnapshot &theme,
                 std::string_view material, std::string_view scheme, unsigned position)
    {
        const auto shape = std::bind_front(
            static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
                &runtime::TextShaper::Shape),
            &shaper_);
        runtime::Scene scene(runtime::ParseBlueprint(source), shape, shaper_.FontId(), theme);
        scene.SetBinding("anchorLead", position == 1 ? metrics_.logical_size.height - 160 : 32.0);
        scene.SetBinding("anchorLeft", position == 2 ? metrics_.logical_size.width - 180 : 140.0);
        scene.SetBinding("muted", false);
        scene.SetViewport(metrics_.logical_size);
        const auto closed = scene.Build({1});
        Require(closed.has_value(), "closed root build failed");
        Draw(parent_, window_.Surface(), *closed, metrics_.buffer_size);
        const auto trigger = FindAction(*scene.InputGeometry(), "sound");
        Require(scene.OpenPopup(trigger), "logical popup failed");
        Require(scene.Build({1}).has_value(), "logical popup build failed");
        const auto root_input = scene.InputGeometry();
        const auto root_bounds = scene.Bounds(trigger);
        const auto request = scene.CapturePopupSurfaceRequest(window_.ConfigureCount());
        Require(request.has_value(), "Scene did not export its request");
        Require(popup_.Open(window_, {request->anchor, request->desired_geometry, request->gap,
                                      request->horizontal_alignment, request->vertical_preference}),
                "native popup failed");
        popup_.SetBeforeCloseHandler(std::bind_front(&Probe::ReleaseChild, this));
        WaitForPopup();
        const auto configure = *popup_.Configure();
        const auto anchor = request->anchor;
        if (position == 0) {
            Require(configure.bounds.y >= anchor.y + anchor.height + request->gap,
                    "below case did not configure below its anchor");
        } else if (position == 1) {
            Require(configure.bounds.y + configure.bounds.height <= anchor.y - request->gap,
                    "above case did not flip above its anchor");
        } else {
            Require(configure.bounds.x < anchor.x + anchor.width / 2 - configure.bounds.width / 2,
                    "edge case did not slide away from the output edge");
        }
        const runtime::PopupSurfaceConfigure final{request->parent_configure_generation,
                                                   configure.generation, configure.bounds};
        std::string diagnostic;
        const auto plan = scene.PreparePopupSurface(*request, final, &diagnostic);
        Require(plan.has_value(), "final Scene preparation failed: " + diagnostic);
        Require(scene.InputGeometry() == root_input && scene.Bounds(trigger) == root_bounds,
                "child preparation changed root geometry");
        Require(popup_.SetBufferLayout(
                    {plan->buffer_size, plan->window_geometry, plan->configure_generation}),
                "native layout rejected the plan");
        Require(child_.Open(context_, popup_.Surface(), int(plan->buffer_size.width),
                            int(plan->buffer_size.height)),
                "child WSI rejected the plan");
        const auto *panel_material = contracts::FindThemeMaterial(theme, "panel");
        Require(panel_material, "theme panel material missing");
        const auto before = PresentPlan(*plan, panel_material->tint.a);

        Require(scene.SetBinding("muted", true), "control binding did not change");
        Require(scene.Build({1}).has_value(), "updated Scene failed to build");
        Require(!scene.PreparePopupSurface(*request, final), "stale request was accepted");
        const auto updated_request = scene.CapturePopupSurfaceRequest(window_.ConfigureCount());
        Require(updated_request.has_value(), "updated request missing");
        const auto updated_plan = scene.PreparePopupSurface(*updated_request, final, &diagnostic);
        Require(updated_plan.has_value(), "updated plan failed: " + diagnostic);
        Require(updated_plan->buffer_size.width == plan->buffer_size.width &&
                    updated_plan->buffer_size.height == plan->buffer_size.height &&
                    updated_plan->window_geometry == plan->window_geometry,
                "binding unexpectedly changed WSI layout");
        const auto after = PresentPlan(*updated_plan, panel_material->tint.a);
        const auto *mute =
            updated_plan->input_snapshot->Find(FindAction(*updated_plan->input_snapshot, "mute"));
        Require(mute, "child subtree omitted checkbox");
        Require(ChangedPixels(before, after, plan->buffer_size, mute->bounds) >= 8,
                "checkbox binding update did not change exported GPU pixels");
        popup_.Close();
        Require(!child_.Ready() && parent_.Ready() && context_.Ready(), "child cleanup failed");
        ++cases_;
        std::cout << "case=" << material << '/' << scheme << '/' << position
                  << " configure=" << configure.bounds.x << ',' << configure.bounds.y << ','
                  << configure.bounds.width << ',' << configure.bounds.height
                  << " buffer=" << plan->buffer_size.width << 'x' << plan->buffer_size.height
                  << " geometry=" << plan->window_geometry.x << ',' << plan->window_geometry.y
                  << ',' << plan->window_geometry.width << ',' << plan->window_geometry.height
                  << '\n';
    }

    void Draw(platform::WaylandEglSurface &target, wl_surface *surface,
              const contracts::DisplayList &list, contracts::BufferSize size)
    {
        Require(target.MakeCurrent(), "target bind failed");
        Require(target.SetDamage(contracts::DamageRegion::Full()) !=
                    platform::DamageRegionResult::Failed,
                "repair declaration failed");
        Require(renderer_->Render(list, target.TargetIdentity(), int(size.width), int(size.height)),
                "GPU draw failed");
        Frame frame(window_, surface);
        Require(target.Swap(), "GPU swap failed");
        ++frames_;
        frame.Wait();
    }

    void Compare(const std::vector<std::uint8_t> &cpu, const std::vector<std::uint8_t> &gpu,
                 contracts::BufferSize size, contracts::LogicalPoint point)
    {
        const int x = int(std::floor(point.x));
        const int y = int(std::floor(point.y));
        Require(x >= 0 && y >= 0 && x < int(size.width) && y < int(size.height),
                "sample outside exported buffer");
        const auto cpu_index = (std::size_t(y) * size.width + x) * 4;
        const auto gpu_index = (std::size_t(size.height - y - 1) * size.width + x) * 4;
        constexpr std::array<std::size_t, 4> channels{2, 1, 0, 3};
        for (std::size_t channel = 0; channel < channels.size(); ++channel) {
            const auto difference =
                std::abs(int(cpu[cpu_index + channels[channel]]) - int(gpu[gpu_index + channel]));
            Require(difference <= 4, "CPU/GPU exported pixels differ at " + std::to_string(x) +
                                         "," + std::to_string(y) +
                                         " channel=" + std::to_string(channel) +
                                         " difference=" + std::to_string(difference));
        }
        ++samples_;
    }

    unsigned ChangedPixels(const std::vector<std::uint8_t> &before,
                           const std::vector<std::uint8_t> &after, contracts::BufferSize size,
                           contracts::LogicalRect area) const
    {
        const int left = std::max(0, int(std::floor(area.x)));
        const int top = std::max(0, int(std::floor(area.y)));
        const int right = std::min(int(size.width), int(std::ceil(area.x + area.width)));
        const int bottom = std::min(int(size.height), int(std::ceil(area.y + area.height)));
        unsigned changed{};
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                const auto offset = (std::size_t(size.height - y - 1) * size.width + x) * 4;
                bool different{};
                for (unsigned channel = 0; channel < 4; ++channel) {
                    different |=
                        std::abs(int(before[offset + channel]) - int(after[offset + channel])) > 4;
                }
                changed += different;
            }
        }
        return changed;
    }

    void VisiblePixel(const std::vector<std::uint8_t> &gpu, contracts::BufferSize size,
                      contracts::LogicalPoint point, std::uint8_t expected_alpha) const
    {
        const auto offset =
            (std::size_t(size.height - unsigned(point.y) - 1) * size.width + unsigned(point.x)) * 4;
        Require(gpu[offset + 3] >= expected_alpha, "exported tinted surface pixel was omitted");
    }

    std::vector<std::uint8_t> PresentPlan(const runtime::PopupSurfacePlan &plan,
                                          std::uint8_t expected_alpha)
    {
        contracts::ValidateDisplayList(*plan.display_list);
        Require(child_.MakeCurrent(), "child bind failed");
        Require(child_.SetDamage(contracts::DamageRegion::Full()) !=
                    platform::DamageRegionResult::Failed,
                "child repair declaration failed");
        const auto size = plan.buffer_size;
        std::vector<std::uint8_t> cpu(std::size_t(size.width) * size.height * 4);
        std::vector<std::uint8_t> gpu(cpu.size());
        Require(raster_.Render(*plan.display_list, cpu.data(), int(size.width), int(size.height),
                               int(size.width) * 4),
                "raster plan failed");
        Require(renderer_->Render(*plan.display_list, child_.TargetIdentity(), int(size.width),
                                  int(size.height)),
                "GPU plan failed");
        glReadPixels(0, 0, int(size.width), int(size.height), GL_RGBA, GL_UNSIGNED_BYTE,
                     gpu.data());
        Require(glGetError() == GL_NO_ERROR, "GPU readback failed");
        const auto body = plan.body_geometry;
        const auto geometry = plan.window_geometry;
        const auto *volume = plan.input_snapshot->Find(FindAction(*plan.input_snapshot, "volume"));
        Require(volume, "child subtree omitted slider");
        const auto track = volume->slider_track;
        const std::array points{
            contracts::LogicalPoint{0, 0},
            contracts::LogicalPoint{body.x + 6, body.y + body.height - 6},
            contracts::LogicalPoint{body.x + body.width / 2, body.y + body.height - 6},
            contracts::LogicalPoint{track.x + track.width / 4, track.y + track.height / 2}};
        for (const auto point : points) {
            Compare(cpu, gpu, size, point);
        }
        VisiblePixel(gpu, size, points[2], expected_alpha);
        const double neck_top = body.y - geometry.y;
        const double neck_bottom = geometry.y + geometry.height - body.y - body.height;
        if (neck_top > 0 || neck_bottom > 0) {
            const auto *panel = plan.input_snapshot->Find(plan.request.active_node);
            Require(panel && panel->contour, "functional attachment contour missing");
            const double y =
                neck_top > 0 ? body.y - neck_top / 2 : body.y + body.height + neck_bottom / 2;
            double first = -1;
            double last = -1;
            for (unsigned x = 0; x < size.width; ++x) {
                if (contracts::ContourContains(*panel->contour, {x + 0.5, y})) {
                    if (first < 0) {
                        first = x + 0.5;
                    }
                    last = x + 0.5;
                }
            }
            Require(first >= 0 && last >= first,
                    "functional attachment missing from local contour");
            const contracts::LogicalPoint neck{(first + last) / 2, y};
            Require(neck.x >= geometry.x && neck.x <= geometry.x + geometry.width &&
                        neck.y >= geometry.y && neck.y <= geometry.y + geometry.height,
                    "functional attachment outside window geometry");
            Compare(cpu, gpu, size, neck);
            VisiblePixel(gpu, size, neck, expected_alpha);
        }

        Frame frame(window_, popup_.Surface());
        Require(child_.Swap(), "child swap failed");
        ++frames_;
        frame.Wait();
        return gpu;
    }

    void ReleaseChild() noexcept
    {
        ++child_hooks_;
        if (renderer_ && child_.Ready()) {
            if (parent_.MakeCurrent() || child_.MakeCurrent()) {
                renderer_->ReleaseTarget(child_.TargetIdentity());
            } else {
                renderer_->Abandon();
            }
        }
        child_.Close();
    }

    void ReleaseGpu() noexcept
    {
        if (child_.Ready()) {
            ReleaseChild();
        }
        if (renderer_) {
            if (!parent_.MakeCurrent()) {
                renderer_->Abandon();
            }
            renderer_.reset();
        }
        parent_.Close();
        context_.Close();
    }

    void Cleanup() noexcept
    {
        popup_.Close();
        window_.Close();
        ReleaseGpu();
    }

    platform::WaylandWindow window_;
    platform::WaylandPopup popup_;
    platform::WaylandEglContext context_;
    platform::WaylandEglSurface parent_, child_;
    runtime::TextShaper shaper_;
    render_skia::RasterRenderer raster_;
    std::unique_ptr<render_skia::GlesRenderer> renderer_;
    contracts::WindowMetrics metrics_;
    std::string driver_;
    unsigned cases_{}, frames_{}, samples_{}, child_hooks_{};
};
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4 || (argc == 4 && std::string(argv[3]) != "--any-gpu")) {
        std::cerr << "usage: scene_popup_surface_probe <socket> <repository> [--any-gpu]\n";
        return 2;
    }
    try {
        Probe probe;
        probe.Run(argv[1], argv[2], argc == 4);
    } catch (const std::exception &error) {
        std::cerr << "scene_popup_surface_probe: " << error.what() << '\n';
        return 1;
    }
}
