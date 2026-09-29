#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

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
    gpu.Close();
    assert(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    assert(eglDestroyContext(display, context));
    assert(eglDestroySurface(display, surface));
    assert(eglTerminate(display));
}
