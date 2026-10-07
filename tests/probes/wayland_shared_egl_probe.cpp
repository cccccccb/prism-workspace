#include "prism/contracts/display_list.hpp"
#include "prism/platform/wayland_egl_context.hpp"
#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/platform/wayland_popup.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/image_resources.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <wayland-client.h>

// Assertions cover explicit owner cleanup. Backend before-close hooks release
// borrowed WSI during unexpected transport teardown; automatic dismissal and
// input/Scene outcomes are outside this probe's assertions.
namespace {
using namespace std::chrono_literals;
using namespace prism;
constexpr contracts::ResourceId image_id{2};
constexpr const char *font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

void Require(bool condition, const char *detail)
{
    if (!condition) {
        throw std::runtime_error(detail);
    }
}

class Frame {
public:
    Frame(platform::WaylandWindow &window, wl_surface *surface)
        : window_(window), display_(window.Display()), callback_(wl_surface_frame(surface))
    {
        Require(callback_, "frame callback creation failed");
        static const wl_callback_listener listener{Done};
        if (wl_callback_add_listener(callback_, &listener, this) != 0) {
            wl_callback_destroy(callback_);
            callback_ = nullptr;
            Require(false, "frame callback listener failed");
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
            Require(std::chrono::steady_clock::now() < deadline, "frame callback timed out");
            Require(window_.Pump(20), "parent pump stopped while waiting for frame");
        }
    }

private:
    static void Done(void *data, wl_callback *callback, std::uint32_t) noexcept
    {
        auto &frame = *static_cast<Frame *>(data);
        wl_callback_destroy(callback);
        frame.callback_ = nullptr;
        frame.done_ = true;
    }

    platform::WaylandWindow &window_;
    wl_display *display_{};
    wl_callback *callback_{};
    bool done_{};
};

contracts::DisplayList Pattern(int width, int height, unsigned seed)
{
    using namespace contracts;
    const Color background{static_cast<std::uint8_t>(20 + seed * 7),
                           static_cast<std::uint8_t>(35 + seed * 5),
                           static_cast<std::uint8_t>(60 + seed * 3), 255};
    DisplayList list;
    list.window = WindowId{1};
    list.generation = std::uint64_t(seed) + 1;
    list.commands = {FillRect{{0, 0, double(width), double(height)}, background},
                     PushClipRoundedRect{{8, 8, double(width - 16), double(height - 16)}, 10},
                     FillRect{{0, 0, double(width), double(height)}, {30, 110, 160, 255}},
                     PushOpacity{0.5},
                     FillRect{{16, 16, double(width / 2), double(height - 32)}, {220, 40, 70, 255}},
                     FillRect{{double(width / 3), 16, double(width / 2), double(height - 32)},
                              {35, 210, 90, 255}},
                     PopOpacity{},
                     PopClip{},
                     DrawImage{image_id, {double(width - 36), 12, 24, 24}, ImageFit::Fill}};
    return list;
}

void ComparePoint(const std::vector<std::uint8_t> &cpu, const std::vector<std::uint8_t> &gpu,
                  int width, int height, int x, int y)
{
    const auto cpu_offset = (std::size_t(y) * width + x) * 4;
    const auto gpu_offset = (std::size_t(height - y - 1) * width + x) * 4;
    constexpr std::array<std::size_t, 4> cpu_channels{2, 1, 0, 3};
    for (std::size_t channel = 0; channel < cpu_channels.size(); ++channel) {
        const auto difference =
            std::abs(int(cpu[cpu_offset + cpu_channels[channel]]) - int(gpu[gpu_offset + channel]));
        Require(difference <= 2, "shared target pixels differ from raster reference");
    }
}

class Probe {
public:
    Probe() : cpu_(font_path)
    {
        Require(cpu_.Ready(), "raster reference did not load font");
    }

    ~Probe()
    {
        Cleanup();
    }

    void Run(const std::string &socket, bool any_gpu)
    {
        window_.SetPaintHandler(std::bind_front(&Probe::BootstrapPixels, this));
        Require(window_.Open(socket, "prism.shared-egl-probe", "Shared EGL popup probe", 480, 320),
                "parent window failed to open");
        WaitForMapping();
        parent_metrics_ = window_.Metrics();
        parent_width_ = int(parent_metrics_.buffer_size.width);
        parent_height_ = int(parent_metrics_.buffer_size.height);
        Require(parent_width_ >= 64 && parent_height_ >= 64, "parent is too small for probe");
        window_.SetSubmitHandlers(std::bind_front(&Probe::IdleSubmit, this), {});
        window_.SetBeforeCloseHandler(std::bind_front(&Probe::BeforeWindowClose, this));

        Require(context_.Open(window_.Display()), "shared context failed to open");
        Require(parent_.Open(context_, window_.Surface(), parent_width_, parent_height_),
                "parent WSI failed to open");
        renderer_name_ = parent_.GlRenderer();
        Require(any_gpu || renderer_name_.find("V3D") != std::string::npos,
                "probe did not use V3D; --any-gpu permits other functional backends");
        native_context_ = eglGetCurrentContext();
        native_display_ = eglGetCurrentDisplay();
        Require(native_context_ != EGL_NO_CONTEXT && native_display_ != EGL_NO_DISPLAY,
                "parent WSI did not bind a context");
        renderer_ =
            std::make_unique<render_skia::GlesRenderer>(font_path, parent_.TargetIdentity());
        Require(renderer_->Ready(), "shared GPU renderer failed to initialize");
        RegisterImage();
        Present(parent_, window_.Surface(), parent_width_, parent_height_, 0);

        OpenChild();

        for (unsigned iteration = 0; iteration < 6; ++iteration) {
            Present(child_, popup_.Surface(), child_width_, child_height_, iteration + 1);
            Require(!popup_.IsClosed(), "popup was dismissed during explicit-lifetime probe");
            Present(parent_, window_.Surface(), parent_width_, parent_height_, iteration + 8);
            Require(!popup_.IsClosed(), "popup was dismissed during explicit-lifetime probe");
        }
        const auto alternating = renderer_->GetRenderStats();
        Require(alternating.target_wraps == 2 && alternating.target_cache_hits >= 11 &&
                    alternating.target_switches >= 11,
                "renderer did not retain and switch both targets");
        Require(alternating.image_upload_successes == 1, "image was uploaded per target");

        Require(parent_.MakeCurrent(), "parent failed to bind before child release");
        const auto parent_draw = eglGetCurrentSurface(EGL_DRAW);
        const auto parent_read = eglGetCurrentSurface(EGL_READ);
        const auto child_target = child_.TargetIdentity();
        Require(renderer_->ReleaseTarget(child_target), "child GPU target release failed");
        child_.Close();
        Require(eglGetCurrentDisplay() == native_display_ &&
                    eglGetCurrentContext() == native_context_ &&
                    eglGetCurrentSurface(EGL_DRAW) == parent_draw &&
                    eglGetCurrentSurface(EGL_READ) == parent_read,
                "child WSI close changed parent binding");
        popup_.Close();
        Require(context_.Ready() && parent_.Ready() && renderer_->Ready(),
                "child close invalidated shared parent resources");
        Require(renderer_->ImageUploaded(image_id), "child close lost shared image texture");
        Present(parent_, window_.Surface(), parent_width_, parent_height_, 15);
        const auto after_close = renderer_->GetRenderStats();
        Require(after_close.target_wraps == alternating.target_wraps &&
                    after_close.target_releases == 1 && after_close.image_upload_successes == 1,
                "parent resources were rebuilt after child close");

        OpenChild();
        Present(child_, popup_.Surface(), child_width_, child_height_, 16);
        const auto before_popup_close = renderer_->GetRenderStats();
        Require(parent_.MakeCurrent(), "parent failed to bind before popup hook close");
        const auto retained_draw = eglGetCurrentSurface(EGL_DRAW);
        popup_.Close();
        Require(popup_.IsClosed() && !child_.Ready() && child_hook_proxy_valid_ &&
                    child_hook_live_wsi_count_ == 1 && child_hook_released_,
                "popup hook did not release live child WSI before proxy teardown");
        Require(renderer_->Ready() && parent_.Ready() && context_.Ready() &&
                    eglGetCurrentContext() == native_context_ &&
                    eglGetCurrentSurface(EGL_DRAW) == retained_draw,
                "popup hook close invalidated parent resources");
        Require(renderer_->GetRenderStats().target_releases ==
                    before_popup_close.target_releases + 1,
                "popup hook did not release the live child GPU target");
        Present(parent_, window_.Surface(), parent_width_, parent_height_, 17);

        OpenChild();
        Present(child_, popup_.Surface(), child_width_, child_height_, 18);
        const auto before_window_close = renderer_->GetRenderStats();
        window_.Close();
        Require(window_hook_calls_ == 1 && window_hook_proxy_valid_ && window_hook_had_live_wsi_ &&
                    window_hook_released_ && !window_.Display() && !window_.Surface(),
                "window hook did not release live GPU resources before native teardown");
        Require(popup_.IsClosed() && !popup_.Surface() && !child_.Ready() && !parent_.Ready() &&
                    !context_.Ready() && !renderer_ && child_hook_calls_ == 3 &&
                    child_hook_proxy_valid_,
                "parent close left a child or shared GPU owner alive");
        Require(shutdown_stats_.target_releases == before_window_close.target_releases + 2,
                "window hook did not release both live GPU targets");
        std::cout << "renderer=" << renderer_name_ << " parent=" << parent_width_ << 'x'
                  << parent_height_ << " child=" << child_width_ << 'x' << child_height_
                  << " frames=" << pixel_frames_ << " wraps=" << shutdown_stats_.target_wraps
                  << " switches=" << shutdown_stats_.target_switches
                  << " cache_hits=" << shutdown_stats_.target_cache_hits
                  << " releases=" << shutdown_stats_.target_releases
                  << " image_uploads=" << shutdown_stats_.image_upload_successes
                  << " parent_age=" << parent_age_ << " child_age=" << child_age_
                  << " window_hooks=" << window_hook_calls_ << " child_hooks=" << child_hook_calls_
                  << '\n';
        Cleanup();
    }

private:
    void BootstrapPixels(void *pixels, int width, int height, int stride)
    {
        for (int y = 0; y < height; ++y) {
            auto *row = reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(pixels) +
                                                          std::size_t(y) * stride);
            std::fill_n(row, width, 0xff203040u);
        }
    }

    platform::SubmitResult IdleSubmit(const platform::SubmitRequest &)
    {
        return platform::SubmitResult::None;
    }

    void WaitForMapping()
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!window_.IsMapped() || window_.FrameCallbackPending()) {
            Require(std::chrono::steady_clock::now() < deadline, "parent mapping timed out");
            Require(window_.Pump(20), "parent pump stopped before mapping");
        }
    }

    void WaitForPopup()
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!popup_.IsConfigured()) {
            Require(!popup_.IsClosed(), "popup closed before configure");
            Require(std::chrono::steady_clock::now() < deadline, "popup configure timed out");
            Require(window_.Pump(20), "parent pump stopped before popup configure");
        }
    }

    void OpenChild()
    {
        const auto logical_width = parent_metrics_.logical_size.width;
        contracts::PopupPositionerRequest request{{logical_width / 2 - 20, 16, 40, 24}, {160, 112}};
        Require(popup_.Open(window_, request), "popup failed to open");
        popup_.SetBeforeCloseHandler(std::bind_front(&Probe::BeforePopupClose, this));
        WaitForPopup();
        Require(popup_.Configure().has_value() && popup_.Surface(), "popup configure missing");
        child_width_ = int(popup_.Configure()->bounds.width);
        child_height_ = int(popup_.Configure()->bounds.height);
        Require(child_width_ >= 64 && child_height_ >= 64, "popup is too small for probe");
        child_native_surface_ = popup_.Surface();
        Require(wl_proxy_get_display(reinterpret_cast<wl_proxy *>(child_native_surface_)) ==
                    window_.Display(),
                "popup used a different Wayland connection");
        Require(child_.Open(context_, child_native_surface_, child_width_, child_height_),
                "child WSI failed to open");
        Require(parent_.TargetIdentity().context_lifetime_id ==
                    child_.TargetIdentity().context_lifetime_id,
                "parent and child did not share context identity");
        Require(parent_.TargetIdentity().surface_lifetime_id !=
                    child_.TargetIdentity().surface_lifetime_id,
                "parent and child reused a surface identity");
    }

    void RegisterImage()
    {
        runtime::DecodedImage image{4, 4, std::vector<std::uint8_t>(4 * 4 * 4)};
        for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4) {
            image.rgba[offset] = 230;
            image.rgba[offset + 1] = 190;
            image.rgba[offset + 2] = 35;
            image.rgba[offset + 3] = 255;
        }
        Require(cpu_.RegisterImage(image_id, image) && renderer_->RegisterImage(image_id, image),
                "shared image registration failed");
        Require(renderer_->UploadImage(image_id), "shared image upload failed");
    }

    void Present(platform::WaylandEglSurface &target, wl_surface *surface, int width, int height,
                 unsigned seed)
    {
        Require(surface && target.MakeCurrent(), "target failed to become current");
        Require(eglGetCurrentDisplay() == native_display_ &&
                    eglGetCurrentContext() == native_context_,
                "target switch changed the shared context");
        const auto age = target.QueryBufferAge();
        if (&target == &parent_) {
            parent_age_ = age.value_or(-1);
        } else {
            child_age_ = age.value_or(-1);
        }
        Require(target.SetDamage(contracts::DamageRegion::Full()) !=
                    platform::DamageRegionResult::Failed,
                "target full repair declaration failed");

        const auto list = Pattern(width, height, seed);
        std::vector<std::uint8_t> cpu(std::size_t(width) * height * 4);
        std::vector<std::uint8_t> gpu(cpu.size());
        Require(cpu_.Render(list, cpu.data(), width, height, width * 4), "raster rendering failed");
        Require(renderer_->Render(list, target.TargetIdentity(), width, height),
                "shared target rendering failed");
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, gpu.data());
        Require(glGetError() == GL_NO_ERROR, "target pixel readback failed");
        const std::array points{std::pair{0, 0},
                                std::pair{12, 12},
                                std::pair{width / 4, height / 2},
                                std::pair{width / 2, height / 2},
                                std::pair{width - 24, 24},
                                std::pair{width - 12, height - 12}};
        for (const auto &[x, y] : points) {
            ComparePoint(cpu, gpu, width, height, x, y);
        }

        Frame frame(window_, surface);
        Require(target.Swap(), "target swap failed");
        ++pixel_frames_;
        frame.Wait();
        const auto metrics = window_.Metrics();
        Require(metrics.logical_size == parent_metrics_.logical_size &&
                    metrics.buffer_size.width == parent_metrics_.buffer_size.width &&
                    metrics.buffer_size.height == parent_metrics_.buffer_size.height,
                "parent resized during fixed-geometry probe");
    }

    void ReleaseChildGpu() noexcept
    {
        if (renderer_ && child_.Ready()) {
            if (parent_.MakeCurrent() || child_.MakeCurrent()) {
                renderer_->ReleaseTarget(child_.TargetIdentity());
            } else {
                renderer_->Abandon();
            }
        }
        child_.Close();
    }

    void BeforePopupClose() noexcept
    {
        ++child_hook_calls_;
        child_hook_proxy_valid_ =
            window_.Display() && window_.Surface() && child_native_surface_ &&
            wl_proxy_get_display(reinterpret_cast<wl_proxy *>(child_native_surface_)) ==
                window_.Display();
        if (child_.Ready()) {
            ++child_hook_live_wsi_count_;
        }
        ReleaseChildGpu();
        child_hook_released_ = !child_.Ready();
        child_native_surface_ = nullptr;
    }

    void BeforeWindowClose() noexcept
    {
        ++window_hook_calls_;
        window_hook_proxy_valid_ =
            window_.Display() && window_.Surface() &&
            wl_proxy_get_display(reinterpret_cast<wl_proxy *>(window_.Surface())) ==
                window_.Display();
        window_hook_had_live_wsi_ = context_.Ready() && parent_.Ready() && child_.Ready() &&
                                    renderer_ && renderer_->Ready();
        ReleaseGpu();
        window_hook_released_ =
            !renderer_ && !child_.Ready() && !parent_.Ready() && !context_.Ready();
    }

    void ReleaseGpu() noexcept
    {
        ReleaseChildGpu();
        if (renderer_) {
            if (parent_.MakeCurrent()) {
                renderer_->ReleaseTarget(parent_.TargetIdentity());
                shutdown_stats_ = renderer_->GetRenderStats();
                renderer_->Close();
            } else {
                renderer_->Abandon();
            }
            renderer_.reset();
        }
        parent_.Close();
        context_.Close();
    }

    void Cleanup() noexcept
    {
        ReleaseGpu();
        popup_.Close();
        window_.SetBeforeCloseHandler({});
        window_.Close();
    }

    platform::WaylandWindow window_;
    platform::WaylandPopup popup_;
    platform::WaylandEglContext context_;
    platform::WaylandEglSurface parent_;
    platform::WaylandEglSurface child_;
    render_skia::RasterRenderer cpu_;
    std::unique_ptr<render_skia::GlesRenderer> renderer_;
    contracts::WindowMetrics parent_metrics_;
    render_skia::GlesRenderStats shutdown_stats_;
    wl_surface *child_native_surface_{};
    EGLDisplay native_display_{EGL_NO_DISPLAY};
    EGLContext native_context_{EGL_NO_CONTEXT};
    std::string renderer_name_;
    int parent_width_{}, parent_height_{}, child_width_{}, child_height_{};
    int parent_age_{-1}, child_age_{-1};
    unsigned pixel_frames_{};
    unsigned child_hook_calls_{}, child_hook_live_wsi_count_{}, window_hook_calls_{};
    bool child_hook_proxy_valid_{}, child_hook_released_{}, window_hook_proxy_valid_{};
    bool window_hook_had_live_wsi_{}, window_hook_released_{};
};
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--any-gpu")) {
        std::cerr << "usage: wayland_shared_egl_probe <socket> [--any-gpu]\n";
        return 2;
    }
    try {
        Probe probe;
        probe.Run(argv[1], argc == 3);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "wayland_shared_egl_probe: " << error.what() << '\n';
        return 1;
    }
}
