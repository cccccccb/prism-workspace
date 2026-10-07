#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace {
using namespace prism::contracts;
using prism::render_skia::GlesRenderer;
using prism::render_skia::RasterRenderer;

std::vector<std::uint8_t> ReadGpu(int width, int height)
{
    std::vector<std::uint8_t> pixels(width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    assert(glGetError() == GL_NO_ERROR);
    return pixels;
}

void CompareContourPoint(const std::vector<std::uint8_t> &cpu, const std::vector<std::uint8_t> &gpu,
                         int width, int height, int x, int y, int tolerance)
{
    const auto *c = &cpu[(y * width + x) * 4];
    const auto *g = &gpu[((height - y - 1) * width + x) * 4];
    assert(std::abs(int(c[2]) - int(g[0])) <= tolerance);
    assert(std::abs(int(c[1]) - int(g[1])) <= tolerance);
    assert(std::abs(int(c[0]) - int(g[2])) <= tolerance);
    assert(std::abs(int(c[3]) - int(g[3])) <= tolerance);
}

void RenderContourPair(const DisplayList &list, RasterRenderer &cpu, GlesRenderer &gpu, int width,
                       int height, std::vector<std::uint8_t> &cpu_pixels,
                       std::vector<std::uint8_t> &gpu_pixels)
{
    assert(cpu.Render(list, cpu_pixels.data(), width, height, width * 4));
    assert(gpu.Render(list, width, height));
    gpu_pixels = ReadGpu(width, height);
}

void CheckContourCases(RasterRenderer &cpu, GlesRenderer &gpu, int width, int height)
{
    // The neck and panel are one concave polygon. A two-part source-over
    // construction would double alpha at the join; this test samples both sides.
    const Contour contour{{{12, 2}, {20, 2}, {20, 6}, {27, 6}, {27, 20}, {5, 20}, {5, 6}, {12, 6}}};
    DisplayList list{{1}, 20, {FillContour{contour, {200, 100, 40, 128}}}};
    std::vector<std::uint8_t> cpu_pixels(width * height * 4), gpu_pixels;
    RenderContourPair(list, cpu, gpu, width, height, cpu_pixels, gpu_pixels);
    for (const auto point : {std::pair{16, 3}, std::pair{16, 5}, std::pair{16, 6},
                             std::pair{16, 12}, std::pair{10, 3}, std::pair{3, 12}}) {
        CompareContourPoint(cpu_pixels, gpu_pixels, width, height, point.first, point.second, 0);
        const auto alpha = cpu_pixels[(point.second * width + point.first) * 4 + 3];
        assert(alpha ==
               (ContourContains(contour, {point.first + 0.5, point.second + 0.5}) ? 128 : 0));
    }

    list.commands = {PushClipContour{contour}, FillRect{{0, 0, width, height}, {200, 100, 40, 128}},
                     PopClip{}};
    RenderContourPair(list, cpu, gpu, width, height, cpu_pixels, gpu_pixels);
    for (const auto point : {std::pair{16, 3}, std::pair{16, 6}, std::pair{16, 12},
                             std::pair{10, 3}, std::pair{3, 12}}) {
        CompareContourPoint(cpu_pixels, gpu_pixels, width, height, point.first, point.second, 0);
        const auto alpha = cpu_pixels[(point.second * width + point.first) * 4 + 3];
        assert(alpha ==
               (ContourContains(contour, {point.first + 0.5, point.second + 0.5}) ? 128 : 0));
    }

    list.commands = {FillContour{contour, {20, 80, 140, 255}},
                     StrokeContour{contour, 2, {240, 240, 240, 255}}};
    RenderContourPair(list, cpu, gpu, width, height, cpu_pixels, gpu_pixels);
    for (const auto point : {std::pair{5, 12}, std::pair{6, 12}, std::pair{16, 12},
                             std::pair{4, 12}, std::pair{16, 21}}) {
        // All points are away from fractional AA coverage. Interior border
        // pixels have the same exact premultiplied color in both backends.
        CompareContourPoint(cpu_pixels, gpu_pixels, width, height, point.first, point.second, 0);
    }
    assert(cpu_pixels[(12 * width + 5) * 4 + 2] == 240);
    assert(cpu_pixels[(12 * width + 16) * 4 + 2] == 20);
    assert(cpu_pixels[(12 * width + 4) * 4 + 3] == 0);
    assert(cpu_pixels[(21 * width + 16) * 4 + 3] == 0);

    list.commands = {ContourShadow{contour, 1, 2, {0, 0, 0, 192}, false}};
    RenderContourPair(list, cpu, gpu, width, height, cpu_pixels, gpu_pixels);
    for (const auto point : {std::pair{16, 12}, std::pair{16, 21}, std::pair{16, 22}}) {
        // Raster and Ganesh approximate Gaussian masks differently. Permit
        // at most 16/255 channel error at selected shadow samples; this is
        // not an AA-edge or whole-image exact-parity claim.
        CompareContourPoint(cpu_pixels, gpu_pixels, width, height, point.first, point.second, 16);
        assert(cpu_pixels[(point.second * width + point.first) * 4 + 3] > 0);
        assert(gpu_pixels[((height - point.second - 1) * width + point.first) * 4 + 3] > 0);
    }
    CompareContourPoint(cpu_pixels, gpu_pixels, width, height, 0, 0, 0);
    assert(cpu_pixels[3] == 0);

    list.commands = {ContourShadow{contour, 1, 1, {0, 0, 0, 192}, true}};
    RenderContourPair(list, cpu, gpu, width, height, cpu_pixels, gpu_pixels);
    CompareContourPoint(cpu_pixels, gpu_pixels, width, height, 5, 12, 16);
    assert(cpu_pixels[(12 * width + 5) * 4 + 3] > 0);
    assert(gpu_pixels[((height - 12 - 1) * width + 5) * 4 + 3] > 0);
    for (const auto point : {std::pair{16, 12}, std::pair{4, 12}, std::pair{16, 21}}) {
        CompareContourPoint(cpu_pixels, gpu_pixels, width, height, point.first, point.second, 0);
        assert(cpu_pixels[(point.second * width + point.first) * 4 + 3] == 0);
    }
    std::cout << "contour GLES: fill/clip neck alpha and interior stroke exact; outer/inset "
                 "Gaussian samples <=16/255 channel delta\n";
}

bool RepairContains(const DamageRegion &repair, int x, int y)
{
    if (repair.full) {
        return true;
    }
    for (const auto &rect : repair.rects) {
        if (x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height) {
            return true;
        }
    }
    return false;
}

void CheckContourRepair(RasterRenderer &cpu, GlesRenderer &gpu, int width, int height)
{
    const Contour contour{{{3, 3}, {5, 3}, {5, 6}, {9, 6}, {9, 13}, {1, 13}, {1, 6}, {3, 6}}};
    DisplayList before{{1},
                       30,
                       {PushTransform{{1.5, 0, 7, 0, 0.6, 5}}, PushOpacity{0.75},
                        ContourShadow{contour, 0.375, 0.25, {0, 0, 0, 192}, false},
                        FillContour{contour, {40, 80, 160, 176}},
                        StrokeContour{contour, 0.75, {220, 230, 240, 160}},
                        ContourShadow{contour, 0.375, 0.25, {0, 0, 0, 96}, true}, PopOpacity{},
                        PopTransform{}}};
    auto after = before;
    after.generation = 31;
    std::get<ContourShadow>(after.commands[2]).blur = 0.5;
    std::get<ContourShadow>(after.commands[2]).offset_y = 0.5;
    std::get<ContourShadow>(after.commands[5]).blur = 0.5;
    std::get<ContourShadow>(after.commands[5]).offset_y = 0.5;
    const auto repair = cpu.CompareDamage(&before, after, width, height, cpu.ResourceEpoch());
    assert(!repair.full && !repair.rects.empty());

    assert(gpu.Render(before, width, height));
    const auto previous_pixels = ReadGpu(width, height);
    const auto stats_before = gpu.GetRenderStats();
    assert(gpu.Render(after, width, height, repair));
    const auto partial_pixels = ReadGpu(width, height);
    assert(gpu.GetRenderStats().partial_renders == stats_before.partial_renders + 1);
    assert(gpu.Render(after, width, height));
    const auto full_pixels = ReadGpu(width, height);

    std::size_t changed_pixels = 0;
    std::size_t preserved_pixels = 0;
    int maximum_delta = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = ((height - y - 1) * width + x) * 4;
            bool changed = false;
            for (int channel = 0; channel < 4; ++channel) {
                changed |= previous_pixels[offset + channel] != full_pixels[offset + channel];
                if (!RepairContains(repair, x, y)) {
                    assert(previous_pixels[offset + channel] == partial_pixels[offset + channel]);
                    assert(previous_pixels[offset + channel] == full_pixels[offset + channel]);
                }
                const auto delta = std::abs(int(partial_pixels[offset + channel]) -
                                            int(full_pixels[offset + channel]));
                // Match existing GLES partial-repair policy: at most one LSB
                // where both pixels have nonzero alpha; zero-alpha stays exact.
                const bool visible = partial_pixels[offset + 3] && full_pixels[offset + 3];
                assert(delta <= (visible ? 1 : 0));
                maximum_delta = std::max(maximum_delta, delta);
            }
            changed_pixels += changed;
            preserved_pixels += !RepairContains(repair, x, y);
        }
    }
    assert(changed_pixels > 0 && preserved_pixels > 0);
    std::cout << "contour GLES repair: changed_pixels=" << changed_pixels
              << " preserved_pixels=" << preserved_pixels << " max_channel_delta=" << maximum_delta
              << " transparent_exact=true\n";
}
} // namespace

int main()
{
    constexpr int width = 32, height = 24;
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    assert(get_platform_display);
    EGLDisplay display =
        get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    assert(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr));
    assert(eglBindAPI(EGL_OPENGL_ES_API));
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE,
                                        EGL_PBUFFER_BIT,
                                        EGL_RENDERABLE_TYPE,
                                        EGL_OPENGL_ES3_BIT,
                                        EGL_RED_SIZE,
                                        8,
                                        EGL_GREEN_SIZE,
                                        8,
                                        EGL_BLUE_SIZE,
                                        8,
                                        EGL_ALPHA_SIZE,
                                        8,
                                        EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    assert(eglChooseConfig(display, config_attributes, &config, 1, &count) && count == 1);
    const EGLint surface_attributes[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    assert(surface != EGL_NO_SURFACE);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    assert(context != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, context));

    const auto *vendor = glGetString(GL_VENDOR);
    const auto *renderer = glGetString(GL_RENDERER);
    assert(vendor && renderer);
    std::cout << "GLES vendor=" << vendor << " renderer=" << renderer << '\n';

    prism::render_skia::RasterRenderer cpu("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    prism::render_skia::GlesRenderer gpu("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    assert(cpu.Ready() && gpu.Ready());
    prism::contracts::DisplayList list;
    list.window = prism::contracts::WindowId{1};
    list.generation = 1;
    list.commands.emplace_back(
        prism::contracts::FillRect{{0, 0, width, height}, {12, 34, 56, 255}});
    list.commands.emplace_back(prism::contracts::PushClipRect{{8, 6, 12, 10}});
    list.commands.emplace_back(
        prism::contracts::FillRect{{0, 0, width, height}, {200, 100, 40, 255}});
    list.commands.emplace_back(prism::contracts::PopClip{});
    std::vector<std::uint8_t> cpu_pixels(width * height * 4), gpu_pixels(width * height * 4);
    assert(cpu.Render(list, cpu_pixels.data(), width, height, width * 4));
    assert(gpu.Render(list, width, height));
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, gpu_pixels.data());
    assert(glGetError() == GL_NO_ERROR);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto *c = &cpu_pixels[(y * width + x) * 4];
            const auto *g = &gpu_pixels[((height - y - 1) * width + x) * 4];
            // CPU target is BGRA; GPU readback is RGBA. Edges are excluded from
            // this exact comparison so backend-specific antialiasing can vary.
            if (x == 8 || x == 19 || y == 6 || y == 15) {
                continue;
            }
            assert(c[2] == g[0] && c[1] == g[1] && c[0] == g[2] && c[3] == g[3]);
        }
    }
    list.commands.clear();
    list.commands.emplace_back(prism::contracts::PushClipRoundedRect{{4, 4, 24, 16}, 6});
    list.commands.emplace_back(
        prism::contracts::FillRect{{0, 0, width, height}, {200, 100, 40, 128}});
    list.commands.emplace_back(prism::contracts::PopClip{});
    assert(cpu.Render(list, cpu_pixels.data(), width, height, width * 4));
    assert(gpu.Render(list, width, height));
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, gpu_pixels.data());
    assert(glGetError() == GL_NO_ERROR);
    for (const auto point : {std::pair{0, 0}, std::pair{16, 12}, std::pair{4, 4}}) {
        const auto *c = &cpu_pixels[(point.second * width + point.first) * 4];
        const auto *g = &gpu_pixels[((height - point.second - 1) * width + point.first) * 4];
        assert(c[2] == g[0] && c[1] == g[1] && c[0] == g[2] && c[3] == g[3]);
    }
    using namespace prism::contracts;
    list.commands = {PushOpacity{0.5}, FillRect{{2, 2, 20, 18}, {255, 0, 0, 255}},
                     FillRect{{10, 2, 20, 18}, {0, 255, 0, 255}}, PopOpacity{}};
    assert(cpu.Render(list, cpu_pixels.data(), width, height, width * 4));
    assert(gpu.Render(list, width, height));
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, gpu_pixels.data());
    assert(glGetError() == GL_NO_ERROR);
    for (const auto point : {std::pair{4, 10}, std::pair{15, 10}, std::pair{27, 10}}) {
        const auto *c = &cpu_pixels[(point.second * width + point.first) * 4];
        const auto *g = &gpu_pixels[((height - point.second - 1) * width + point.first) * 4];
        assert(c[3] == 128 && g[3] == 128);
        assert(c[2] == g[0] && c[1] == g[1] && c[0] == g[2]);
        if (point.first > 10) {
            assert(c[2] == 0 && c[1] == 128 && c[0] == 0);
        }
    }
    CheckContourCases(cpu, gpu, width, height);
    CheckContourRepair(cpu, gpu, width, height);
    gpu.Close();
    assert(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    assert(eglDestroyContext(display, context));
    assert(eglDestroySurface(display, surface));
    assert(eglTerminate(display));
}
