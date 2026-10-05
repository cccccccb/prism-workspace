#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/render_skia/gles_renderer.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/buffer_damage.hpp"
#include "prism/runtime/text_shaper.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace prism::contracts;
using prism::render_skia::GlesRenderer;
using prism::render_skia::RasterRenderer;
using prism::runtime::BufferDamageHistory;
using prism::runtime::TextShaper;
constexpr BufferSize initial_size{384, 256};
constexpr ResourceId image_id{71}, serif_font{72};
constexpr auto default_font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
constexpr auto serif_font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf";

[[noreturn]] void Fail(const std::string &detail)
{
    std::cerr << "skia_damage_test: " << detail << '\n';
    std::abort();
}

void Require(bool condition, const std::string &detail)
{
    if (!condition) {
        Fail(detail);
    }
}

prism::runtime::DecodedImage Image(unsigned variant)
{
    prism::runtime::DecodedImage image;
    image.width = 9;
    image.height = 5;
    image.rgba.resize(image.width * image.height * 4);
    for (unsigned y = 0; y < image.height; ++y) {
        for (unsigned x = 0; x < image.width; ++x) {
            auto *pixel = &image.rgba[(y * image.width + x) * 4];
            pixel[0] = static_cast<std::uint8_t>(23 + x * 23 + variant * 31);
            pixel[1] = static_cast<std::uint8_t>(212 - y * 37 + variant * 17);
            pixel[2] = static_cast<std::uint8_t>((x + y + variant) % 2 ? 219 : 37);
            pixel[3] = static_cast<std::uint8_t>((x + 2 * y + variant) % 4 ? 181 : 0);
        }
    }
    return image;
}

DrawGlyphRun Text(TextShaper &shaper, ResourceId font, std::string_view text, double size, double x,
                  double y, Color color)
{
    auto shaped = shaper.Shape(font, text, size);
    Require(!shaped.glyphs.empty(), "font shaping failed");
    for (auto &glyph : shaped.glyphs) {
        glyph.origin.x += x;
        glyph.origin.y += y;
    }
    return {font, std::move(shaped.glyphs), color, size};
}

enum class ExpectedDamage { Partial, Full, Empty };

struct Frame {
    std::string name;
    DisplayList list;
    ExpectedDamage expected{ExpectedDamage::Partial};
    std::optional<unsigned> image_variant;
    std::optional<BufferSize> resize;
    bool reset_epoch{false};
    std::optional<unsigned> age_override;
    bool unknown_age{false};
    bool full_repair{false};
};

std::vector<Frame> Frames(TextShaper &shaper)
{
    DisplayList initial;
    initial.window = {1};
    initial.generation = 1;
    initial.commands = {
        FillRect{{0, 0, 210, 124}, {12, 20, 38, 130}},
        FillRect{{16.25, 12.5, 68, 36}, {205, 76, 38, 192}},
        FillRoundedRect{{44.5, 28.25, 88, 60}, 9.5, {40, 130, 224, 105}},
        StrokeRoundedRect{{150.25, 18.5, 70.5, 48.5}, 12, 2.25, {211, 242, 254, 230}},
        RoundedRectShadow{{154, 80, 60, 40}, 10, 5.5, 6.25, {22, 11, 37, 115}, false},
        FillRoundedRect{{238, 79, 96, 54}, 12, {93, 173, 211, 85}},
        RoundedRectShadow{{238, 79, 96, 54}, 12, 3, 5, {30, 13, 61, 135}, true},
        DrawImage{image_id, {278.5, 12.25, 68, 48}, ImageFit::Contain},
        Text(shaper, shaper.FontId(), "Prism gypj", 18.5, 12.25, 127.5, {251, 207, 98, 208}),
        PushClipRect{{220.25, 143.5, 118.5, 89}},
        FillRect{{208, 135, 120, 38}, {121, 35, 223, 119}},
        DrawImage{image_id, {312, 142, 32, 40}, ImageFit::Cover},
        PopClip{},
        PushClipRoundedRect{{344.25, 70.25, 31.5, 73.5}, 10},
        FillRect{{331, 56, 50, 107}, {79, 235, 133, 91}},
        PopClip{},
        PushTransform{},
        FillRect{{350, 186, 22, 43}, {213, 65, 155, 163}},
        PopTransform{}};
    constexpr auto first_icon = std::size_t{19};
    const auto icon_count = static_cast<unsigned>(VectorIcon::HeartOutline) + 1;
    for (unsigned icon = 0; icon < icon_count; ++icon) {
        initial.commands.emplace_back(
            DrawIcon{static_cast<VectorIcon>(icon),
                     {10.0 + (icon % 12) * 30, 156.0 + (icon / 12) * 24, 22, 22},
                     {140, 220, 230, 170}});
    }
    const auto group = initial.commands.size();
    const DrawCommand group_commands[] = {
        PushTransform{},
        PushOpacity{1},
        RoundedRectShadow{{50, 60, 85, 46}, 8, 4, 3, {8, 12, 24, 140}, false},
        PushClipRect{{50.25, 58.25, 95.5, 65.5}},
        FillRect{{48, 56, 64, 50}, {225, 55, 95, 255}},
        PushTransform{},
        PushOpacity{0.65},
        FillRoundedRect{{68, 70, 50, 42}, 7, {45, 200, 160, 255}},
        FillRect{{85, 76, 35, 22}, {70, 120, 230, 255}},
        DrawIcon{VectorIcon::Music, {81, 81, 15, 15}, {255, 255, 255, 255}},
        Text(shaper, shaper.FontId(), "gyp", 13.5, 70, 103, {230, 235, 240, 220}),
        DrawImage{image_id, {102, 72, 13, 13}, ImageFit::Contain},
        PopOpacity{},
        PopTransform{},
        PopClip{},
        PopOpacity{},
        PopTransform{}};
    initial.commands.insert(initial.commands.end(), std::begin(group_commands),
                            std::end(group_commands));
    std::vector<Frame> frames;
    Frame first;
    first.name = "initial";
    first.list = std::move(initial);
    first.expected = ExpectedDamage::Full;
    frames.push_back(std::move(first));
    const auto add = [&](std::string name,
                         ExpectedDamage expected = ExpectedDamage::Partial) -> Frame & {
        Frame frame;
        frame.name = std::move(name);
        frame.list = frames.back().list;
        ++frame.list.generation;
        frame.expected = expected;
        frames.push_back(std::move(frame));
        return frames.back();
    };
    add("generation only", ExpectedDamage::Empty);
    std::get<FillRect>(add("same-count position move").list.commands[1]).bounds = {92.75, 7.25, 68,
                                                                                   36};
    std::get<FillRect>(add("transparent erase").list.commands[1]).color.a = 0;
    auto &restore = std::get<FillRect>(add("restore translucent source-over").list.commands[1]);
    restore.bounds = {21.5, 39.25, 68, 36};
    restore.color = {215, 49, 109, 73};
    for (unsigned step = 0; step < 7; ++step) {
        auto &overlap = std::get<FillRoundedRect>(
            add("overlap alpha " + std::to_string(step)).list.commands[2]);
        overlap.bounds.x = 39.25 + step * 3.5;
        overlap.radius = 4.25 + step * 1.5;
        overlap.color.a = static_cast<std::uint8_t>(37 + step * 21);
    }
    auto &stroke = std::get<StrokeRoundedRect>(add("stroke width and alpha").list.commands[3]);
    stroke.bounds = {145.75, 14.25, 77.5, 58.25};
    stroke.width = 8.5;
    stroke.radius = 16.25;
    stroke.color.a = 77;
    auto &shadow = std::get<RoundedRectShadow>(add("outer blur negative offset").list.commands[4]);
    shadow.blur = 9;
    shadow.offset_y = -5.75;
    shadow.bounds.x += 9.5;
    shadow.color = {122, 45, 203, 153};
    auto &inset = std::get<RoundedRectShadow>(add("inset blur negative offset").list.commands[6]);
    inset.blur = 7;
    inset.offset_y = -7.5;
    inset.radius = 22;
    inset.color.a = 51;
    // The hard clip starts at x=220.25. Its stable integer coverage includes
    // pixel column 220; this entire narrow ink is left of the logical edge.
    // Bounds comparison must use the same outward clip as replay.
    std::get<FillRect>(add("fractional hard clip fringe ink").list.commands[10]).bounds = {
        220.05, 145.25, 0.15, 8.25};
    std::get<FillRect>(add("fractional hard clip fringe recolor").list.commands[10]).color = {
        247, 31, 107, 173};
    // Rounded clips retain AA coverage. Guard the ink before intersection:
    // these geometric bounds end before the clip but share its boundary pixel.
    std::get<FillRect>(add("rounded AA clip fringe ink").list.commands[14]).bounds = {
        344.05, 101.25, 0.15, 8.25};
    std::get<FillRect>(add("rounded AA clip fringe recolor").list.commands[14]).color = {227, 52,
                                                                                         149, 211};
    add("glyph font size and descenders").list.commands[8] =
        Text(shaper, serif_font, "jgy", 28.5, 6.75, 118.25, {182, 247, 245, 121});
    add("glyph long-to-short ink").list.commands[8] =
        Text(shaper, shaper.FontId(), "Quick gypsy fjord", 14.25, 14, 134.5, {237, 200, 178, 218});
    add("glyph short with negative origin").list.commands[8] =
        Text(shaper, serif_font, "j", 31.25, -2.5, 119.75, {188, 122, 250, 88});
    for (unsigned icon = 0; icon < icon_count; ++icon) {
        auto &value = std::get<DrawIcon>(
            add("vector path " + std::to_string(icon)).list.commands[first_icon + icon]);
        value.bounds.x += 3.25;
        value.bounds.y -= 2.5;
        value.bounds.width = 24.25;
        value.bounds.height = 20.5;
        value.color = {251, 104, 61, static_cast<std::uint8_t>(70 + icon * 5)};
    }
    std::get<DrawImage>(add("image contain-to-cover").list.commands[7]).fit = ImageFit::Cover;
    auto &image = std::get<DrawImage>(add("image cover-to-fill move").list.commands[7]);
    image.fit = ImageFit::Fill;
    image.destination = {269.25, 16.5, 72.25, 42.5};
    auto &resource = add("same-id image resource replacement", ExpectedDamage::Full);
    resource.image_variant = 1;
    add("resource epoch stable", ExpectedDamage::Empty);
    std::get<PushClipRect>(
        add("rect clip change full fallback", ExpectedDamage::Full).list.commands[9])
        .bounds.x -= 12;
    std::get<PushClipRoundedRect>(
        add("rounded clip change full fallback", ExpectedDamage::Full).list.commands[13])
        .radius = 4.25;
    std::get<PushTransform>(
        add("affine transform full fallback", ExpectedDamage::Full).list.commands[16])
        .values = {0.95, 0.12, -12.5, -0.1, 1.05, 5};
    add("unchanged arbitrary affine full fallback", ExpectedDamage::Full);
    add("return from arbitrary affine full fallback", ExpectedDamage::Full).list.commands[16] =
        PushTransform{};
    std::get<PushOpacity>(add("group opacity overlap").list.commands[group + 1]).opacity = 0.5;
    std::get<PushTransform>(add("scaled group with clipped shadow").list.commands[group]).values = {
        1.13, 0, -4.25, 0, 1.07, 3.5};
    std::get<PushOpacity>(add("nested opacity").list.commands[group + 6]).opacity = 0.23;
    std::get<PushTransform>(add("nested transform").list.commands[group + 5]).values = {
        0.8, 0, 18.5, 0, 1.4, -27.25};
    auto &group_shadow =
        std::get<RoundedRectShadow>(add("transformed shadow ink changes").list.commands[group + 2]);
    group_shadow.blur = 8;
    group_shadow.offset_y = -6;
    std::get<PushTransform>(add("nonuniform scale radial shadow").list.commands[group]).values = {
        1.9, 0, -20, 0, 0.45, 60};
    std::get<PushOpacity>(add("group opacity erases subtree").list.commands[group + 1]).opacity = 0;
    add("transparent group unchanged", ExpectedDamage::Empty);
    std::get<PushTransform>(
        add("transparent group moves without damage", ExpectedDamage::Empty).list.commands[group])
        .values = {1.08, 0, 12.5, 0, 0.83, 15.25};
    std::get<PushOpacity>(add("group opacity restores moved subtree").list.commands[group + 1])
        .opacity = 1;
    add("group motion idle", ExpectedDamage::Empty);
    add("command structure full fallback", ExpectedDamage::Full)
        .list.commands.emplace_back(FillRect{{333, 235, 27, 12}, {34, 245, 108, 79}});
    add("command removal full fallback", ExpectedDamage::Full).list.commands.pop_back();
    auto &age_zero = add("age zero repairs unchanged content", ExpectedDamage::Empty);
    age_zero.age_override = 0;
    age_zero.full_repair = true;
    auto &unknown = add("unknown age repairs unchanged content", ExpectedDamage::Empty);
    unknown.unknown_age = true;
    unknown.full_repair = true;
    auto &too_old = add("lost damage history full repair");
    std::get<FillRect>(too_old.list.commands[1]).color.a = 111;
    too_old.age_override = 99;
    too_old.full_repair = true;
    auto &resize = add("resize invalidates buffer epoch", ExpectedDamage::Empty);
    resize.resize = BufferSize{416, 272};
    resize.full_repair = true;
    auto &new_epoch = add("same-size new target epoch", ExpectedDamage::Empty);
    new_epoch.reset_epoch = true;
    new_epoch.full_repair = true;
    // After restoration, every rotation reaches an empty repair. No alpha may
    // accumulate, and no untouched pixel may be cleared by an empty replay.
    for (unsigned i = 0; i < 10; ++i) {
        add("unchanged restored target " + std::to_string(i), ExpectedDamage::Empty);
    }
    return frames;
}

class EglDisplay {
public:
    EglDisplay()
    {
        const auto platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        Require(platform_display != nullptr, "surfaceless display entry point unavailable");
        display = platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr),
                "EGL initialize failed");
        Require(eglBindAPI(EGL_OPENGL_ES_API), "EGL GLES bind failed");
        const EGLint attributes[] = {EGL_SURFACE_TYPE,
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
                                     EGL_DEPTH_SIZE,
                                     0,
                                     EGL_STENCIL_SIZE,
                                     8,
                                     EGL_NONE};
        EGLint count = 0;
        Require(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1,
                "RGBA8 GLES3 pbuffer config unavailable");
    }

    ~EglDisplay()
    {
        Require(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT),
                "unbind EGL display failed");
        Require(eglTerminate(display), "terminate EGL display failed");
    }

    EGLDisplay display{EGL_NO_DISPLAY};
    EGLConfig config{};
};

class GpuTarget {
public:
    GpuTarget(EglDisplay &owner, BufferSize size) : owner_(owner)
    {
        const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(owner_.display, owner_.config, EGL_NO_CONTEXT, attributes);
        Require(context_ != EGL_NO_CONTEXT, "independent EGL context creation failed");
        Recreate(size);
    }

    ~GpuTarget()
    {
        Current();
        renderer_->Close();
        renderer_.reset();
        Require(eglMakeCurrent(owner_.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT),
                "target unbind failed");
        Require(eglDestroyContext(owner_.display, context_), "target context destroy failed");
        Require(eglDestroySurface(owner_.display, surface_), "target pbuffer destroy failed");
    }

    void Current()
    {
        Require(eglMakeCurrent(owner_.display, surface_, surface_, context_),
                "make target current failed");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Require(glGetError() == GL_NO_ERROR, "default framebuffer bind failed");
    }

    void Recreate(BufferSize size)
    {
        if (surface_ != EGL_NO_SURFACE) {
            Current();
            renderer_->Close();
            renderer_.reset();
            Require(eglMakeCurrent(owner_.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT),
                    "recreate unbind failed");
            Require(eglDestroySurface(owner_.display, surface_), "recreate pbuffer destroy failed");
        }
        const EGLint attributes[] = {EGL_WIDTH, static_cast<EGLint>(size.width), EGL_HEIGHT,
                                     static_cast<EGLint>(size.height), EGL_NONE};
        surface_ = eglCreatePbufferSurface(owner_.display, owner_.config, attributes);
        Require(surface_ != EGL_NO_SURFACE, "pbuffer creation failed");
        Current();
        renderer_ = std::make_unique<GlesRenderer>(default_font_path);
        Require(renderer_->Ready(), "Ganesh initialization failed");
        Require(renderer_->RegisterFont(serif_font, serif_font_path),
                "GPU alternate font registration failed");
        Require(renderer_->RegisterImage(image_id, Image(image_variant_)),
                "GPU image registration failed");
    }

    void SetImage(unsigned variant)
    {
        image_variant_ = variant;
        Current();
        Require(renderer_->RegisterImage(image_id, Image(variant)), "GPU image replacement failed");
    }

    GlesRenderer &Renderer()
    {
        return *renderer_;
    }

    std::vector<std::uint8_t> Read(BufferSize size)
    {
        Current();
        std::vector<std::uint8_t> bottom_left(static_cast<std::size_t>(size.width) * size.height *
                                              4);
        glReadPixels(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height),
                     GL_RGBA, GL_UNSIGNED_BYTE, bottom_left.data());
        Require(glGetError() == GL_NO_ERROR, "GPU readback failed");
        std::vector<std::uint8_t> top_left(bottom_left.size());
        for (unsigned y = 0; y < size.height; ++y) {
            for (unsigned x = 0; x < size.width; ++x) {
                for (unsigned channel = 0; channel < 4; ++channel) {
                    top_left[(static_cast<std::size_t>(y) * size.width + x) * 4 + channel] =
                        bottom_left[(static_cast<std::size_t>(size.height - y - 1) * size.width +
                                     x) *
                                        4 +
                                    channel];
                }
            }
        }
        return top_left;
    }

private:
    EglDisplay &owner_;
    unsigned image_variant_{};
    EGLContext context_{EGL_NO_CONTEXT};
    EGLSurface surface_{EGL_NO_SURFACE};
    std::unique_ptr<GlesRenderer> renderer_;
};

void ComparePixels(const std::vector<std::uint8_t> &partial, const std::vector<std::uint8_t> &full,
                   BufferSize size, bool bgra, const std::string &label)
{
    Require(partial.size() == full.size(), label + " mismatched readback sizes");
    for (unsigned y = 0; y < size.height; ++y) {
        for (unsigned x = 0; x < size.width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * size.width + x) * 4;
            bool equal = true;
            for (unsigned channel = 0; channel < 4; ++channel) {
                equal &= partial[index + channel] == full[index + channel];
            }
            if (equal) {
                continue;
            }
            const std::array<unsigned, 4> channels =
                bgra ? std::array<unsigned, 4>{2, 1, 0, 3} : std::array<unsigned, 4>{0, 1, 2, 3};
            std::cerr << label << " first mismatch at (" << x << ',' << y << ") partial RGBA=(";
            for (unsigned channel = 0; channel < 4; ++channel) {
                std::cerr << (channel ? "," : "") << unsigned(partial[index + channels[channel]]);
            }
            std::cerr << ") full RGBA=(";
            for (unsigned channel = 0; channel < 4; ++channel) {
                std::cerr << (channel ? "," : "") << unsigned(full[index + channels[channel]]);
            }
            std::cerr << ")\n";
            Fail("entire-target exact pixel comparison failed");
        }
    }
}

bool ContainsPixel(const DamageRegion &region, unsigned x, unsigned y)
{
    if (region.full) {
        return true;
    }
    for (const auto &rect : region.rects) {
        if (static_cast<std::int64_t>(x) >= rect.x && static_cast<std::int64_t>(y) >= rect.y &&
            static_cast<std::int64_t>(x) < static_cast<std::int64_t>(rect.x) + rect.width &&
            static_cast<std::int64_t>(y) < static_cast<std::int64_t>(rect.y) + rect.height) {
            return true;
        }
    }
    return false;
}

void CompareOutside(const std::vector<std::uint8_t> &before, const std::vector<std::uint8_t> &after,
                    BufferSize size, const DamageRegion &region, const std::string &label)
{
    Require(before.size() == after.size(), label + " mismatched readback sizes");
    for (unsigned y = 0; y < size.height; ++y) {
        for (unsigned x = 0; x < size.width; ++x) {
            if (ContainsPixel(region, x, y)) {
                continue;
            }
            const auto index = (static_cast<std::size_t>(y) * size.width + x) * 4;
            for (unsigned channel = 0; channel < 4; ++channel) {
                if (before[index + channel] != after[index + channel]) {
                    std::cerr << label << " outside-region mismatch at (" << x << ',' << y
                              << ") channel=" << channel
                              << " before=" << unsigned(before[index + channel])
                              << " after=" << unsigned(after[index + channel]) << '\n';
                    Fail("outside-region byte-exact comparison failed");
                }
            }
        }
    }
}

void CompareGpuPixels(const std::vector<std::uint8_t> &partial,
                      const std::vector<std::uint8_t> &full, BufferSize size,
                      const std::string &label)
{
    Require(partial.size() == full.size(), label + " mismatched readback sizes");
    std::uint64_t toleranced_pixels = 0;
    unsigned maximum_rgba_delta = 0, maximum_alpha_delta = 0;
    // Ganesh crops textured quads and recomputes float texture coordinates
    // (SurfaceDrawContext::attemptQuadOptimization / GrQuadUtils::crop_simple_rect).
    // Only RGBA quantization when both pixels are nontransparent may differ
    // by one LSB. Zero/nonzero alpha transitions remain byte-exact.
    // Separate checks below prove
    // no writes outside repair and no source changes outside content damage.
    for (unsigned y = 0; y < size.height; ++y) {
        for (unsigned x = 0; x < size.width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * size.width + x) * 4;
            unsigned rgba_delta = 0;
            for (unsigned channel = 0; channel < 4; ++channel) {
                rgba_delta = std::max(rgba_delta,
                                      static_cast<unsigned>(std::abs(int(partial[index + channel]) -
                                                                     int(full[index + channel]))));
            }
            const auto alpha_delta =
                static_cast<unsigned>(std::abs(int(partial[index + 3]) - int(full[index + 3])));
            const bool valid =
                partial[index + 3] && full[index + 3] ? rgba_delta <= 1 : rgba_delta == 0;
            if (!valid) {
                std::cerr << label << " numeric-policy mismatch at (" << x << ',' << y
                          << ") partial RGBA=(";
                for (unsigned channel = 0; channel < 4; ++channel) {
                    std::cerr << (channel ? "," : "") << unsigned(partial[index + channel]);
                }
                std::cerr << ") full RGBA=(";
                for (unsigned channel = 0; channel < 4; ++channel) {
                    std::cerr << (channel ? "," : "") << unsigned(full[index + channel]);
                }
                std::cerr << ")\n";
                Fail("GPU bounded-RGBA/exact-transparent comparison failed");
            }
            if (rgba_delta) {
                ++toleranced_pixels;
            }
            maximum_rgba_delta = std::max(maximum_rgba_delta, rgba_delta);
            maximum_alpha_delta = std::max(maximum_alpha_delta, alpha_delta);
        }
    }
    std::cout << label << " toleranced_pixels=" << toleranced_pixels
              << " max_rgba_delta=" << maximum_rgba_delta
              << " max_alpha_delta=" << maximum_alpha_delta << " transparent_exact=true\n";
}

void CheckLargeRepairDoesNotExpand(EglDisplay &display, RasterRenderer &commands)
{
    constexpr BufferSize size{100, 64};
    constexpr Color sentinel{197, 61, 143, 255};
    constexpr Color changed{21, 187, 71, 93};
    DisplayList before{{1}, 1, {FillRect{{0, 0, 100, 64}, sentinel}}};
    DisplayList next{{1}, 2, {FillRect{{0, 0, 100, 64}, changed}}};
    // This exact 80% declaration models an already submitted WSI repair
    // region. It must not be upgraded by a producer's 75% full-frame policy.
    const DamageRegion repair{false, {{0, 0, 80, 64}}};
    // Independently authored full reference: outside the declared repair is
    // opaque sentinel, while repaired pixels contain only the new alpha ink.
    DisplayList expected{
        {1}, 3, {FillRect{{0, 0, 80, 64}, changed}, FillRect{{80, 0, 20, 64}, sentinel}}};
    std::vector<std::uint8_t> cpu_partial(size.width * size.height * 4, 0xcd);
    std::vector<std::uint8_t> cpu_full(cpu_partial.size());
    Require(commands.Render(before, cpu_partial.data(), size.width, size.height, size.width * 4),
            "80% sentinel CPU initialization failed");
    Require(
        commands.Render(next, cpu_partial.data(), size.width, size.height, size.width * 4, repair),
        "80% declared CPU repair failed");
    Require(commands.Render(expected, cpu_full.data(), size.width, size.height, size.width * 4),
            "80% sentinel CPU reference failed");
    ComparePixels(cpu_partial, cpu_full, size, true, "80% repair outside sentinel CPU");
    GpuTarget partial(display, size), full(display, size);
    partial.Current();
    Require(partial.Renderer().Render(before, size.width, size.height),
            "80% sentinel GPU initialization failed");
    Require(partial.Renderer().Render(next, size.width, size.height, repair),
            "80% declared GPU repair failed");
    const auto actual = partial.Read(size);
    full.Current();
    Require(full.Renderer().Render(expected, size.width, size.height),
            "80% sentinel GPU reference failed");
    ComparePixels(actual, full.Read(size), size, false, "80% repair outside sentinel GLES");
    std::cout << "80% directly declared repair preserves outside sentinel (CPU/GLES exact)\n";
}

void RunRotation(EglDisplay &display, RasterRenderer &commands, const std::vector<Frame> &frames,
                 unsigned buffer_count)
{
    Require(commands.RegisterImage(image_id, Image(0)), "initial image registration failed");
    BufferSize size = initial_size;
    BufferDamageHistory history;
    history.Reset(size);
    std::array<std::unique_ptr<GpuTarget>, 3> targets;
    for (auto &target : targets) {
        target = std::make_unique<GpuTarget>(display, size);
    }
    // This fourth independent context always clears/replays the full list. It
    // never consumes CompareDamage, repair rectangles, or a partial result.
    GpuTarget reference(display, size);
    reference.Current();
    const auto *driver = glGetString(GL_RENDERER);
    Require(driver != nullptr, "GL renderer string unavailable");
    std::cout << "rotation=" << buffer_count << " GL_RENDERER=" << driver
              << " (simulated buffer ages; independent pbuffer/context per target)\n";
    std::array<std::vector<std::uint8_t>, 3> cpu_targets;
    const auto poison_cpu = [&] {
        for (auto &pixels : cpu_targets) {
            pixels.assign(static_cast<std::size_t>(size.width) * size.height * 4, 0xcd);
        }
    };
    poison_cpu();
    std::array<std::uint64_t, 3> last_sequence{};
    std::array<bool, 3> initialized{};
    const DisplayList *previous = nullptr;
    std::uint64_t previous_resource_epoch = commands.ResourceEpoch();
    std::uint64_t partial_count = 0, full_count = 0, empty_count = 0;
    std::vector<std::uint8_t> previous_full_reference;
    BufferSize previous_reference_size{};
    for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
        const auto &frame = frames[frame_index];
        const auto label = "rotation=" + std::to_string(buffer_count) + " frame=" + frame.name;
        if (frame.image_variant) {
            Require(previous && previous->commands == frame.list.commands,
                    label + " resource replacement must not mutate display commands");
            const auto before_epoch = commands.ResourceEpoch();
            Require(commands.RegisterImage(image_id, Image(*frame.image_variant)),
                    label + " image replacement failed");
            Require(commands.ResourceEpoch() > before_epoch,
                    label + " missing resource epoch advancement");
            for (auto &target : targets) {
                target->SetImage(*frame.image_variant);
            }
            reference.SetImage(*frame.image_variant);
        }
        if (frame.resize || frame.reset_epoch) {
            const auto old_epoch = history.Epoch();
            if (frame.resize) {
                size = *frame.resize;
            }
            history.Reset(size);
            Require(history.Epoch() != old_epoch, label + " missing buffer epoch advancement");
            initialized.fill(false);
            for (auto &target : targets) {
                target->Recreate(size);
            }
            reference.Recreate(size);
            poison_cpu();
        }
        const auto damage =
            commands.CompareDamage(previous, frame.list, static_cast<int>(size.width),
                                   static_cast<int>(size.height), previous_resource_epoch);
        if (frame.expected == ExpectedDamage::Full) {
            Require(damage.full, label + " expected conservative full content damage");
        } else if (frame.expected == ExpectedDamage::Empty) {
            Require(!damage.full && damage.rects.empty(), label + " expected unchanged content");
        } else {
            Require(!damage.full && !damage.rects.empty(),
                    label + " expected actual partial content damage");
        }
        const auto slot = frame_index % buffer_count;
        // Ages are derived from the simulated target's last successful use,
        // never queried from a pbuffer or claimed as real WSI buffer ages.
        std::optional<unsigned> age =
            initialized[slot]
                ? static_cast<unsigned>(history.SuccessfulSequence() + 1 - last_sequence[slot])
                : 0;
        if (frame.age_override) {
            age = frame.age_override;
        }
        if (frame.unknown_age) {
            age.reset();
        }
        auto plan = history.Plan(damage, age);
        if (frame.full_repair) {
            Require(plan.repair_damage.full, label + " expected complete buffer repair");
        }
        if (plan.repair_damage.full) {
            ++full_count;
        } else if (plan.repair_damage.rects.empty()) {
            ++empty_count;
        } else {
            ++partial_count;
        }
        auto &cpu_partial = cpu_targets[slot];
        std::vector<std::uint8_t> cpu_full(cpu_partial.size(), 0xbe);
        Require(commands.Render(frame.list, cpu_partial.data(), size.width, size.height,
                                size.width * 4, plan.repair_damage),
                label + " CPU partial render failed");
        Require(
            commands.Render(frame.list, cpu_full.data(), size.width, size.height, size.width * 4),
            label + " CPU full reference failed");
        ComparePixels(cpu_partial, cpu_full, size, true, label + " CPU");
        auto &gpu_partial = *targets[slot];
        gpu_partial.Current();
        const auto before_gpu = gpu_partial.Read(size);
        Require(
            gpu_partial.Renderer().Render(frame.list, size.width, size.height, plan.repair_damage),
            label + " GPU partial render failed");
        auto actual = gpu_partial.Read(size);
        CompareOutside(before_gpu, actual, size, plan.repair_damage,
                       label + " GLES repair preservation");
        reference.Current();
        Require(reference.Renderer().Render(frame.list, size.width, size.height),
                label + " GPU full reference failed");
        auto full_reference = reference.Read(size);
        if (!previous_full_reference.empty() && previous_reference_size.width == size.width &&
            previous_reference_size.height == size.height) {
            CompareOutside(previous_full_reference, full_reference, size, damage,
                           label + " independent full-reference content coverage");
        }
        CompareGpuPixels(actual, full_reference, size, label + " GLES");
        previous_full_reference = std::move(full_reference);
        previous_reference_size = size;
        const auto sequence = plan.next_sequence;
        Require(history.Commit(std::move(plan)),
                label + " successful simulated presentation failed to publish history");
        last_sequence[slot] = sequence;
        initialized[slot] = true;
        previous = &frame.list;
        previous_resource_epoch = commands.ResourceEpoch();
    }
    Require(partial_count > 0 && full_count > 0 && empty_count > 0,
            "rotation must exercise partial, full, and empty replay");
    std::cout << "rotation=" << buffer_count << " validated frames=" << frames.size()
              << " partial=" << partial_count << " full=" << full_count << " empty=" << empty_count
              << '\n';
}

std::vector<std::uint8_t> ReadNative(BufferSize size)
{
    std::vector<std::uint8_t> bottom_left(static_cast<std::size_t>(size.width) * size.height * 4);
    glReadPixels(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height), GL_RGBA,
                 GL_UNSIGNED_BYTE, bottom_left.data());
    Require(glGetError() == GL_NO_ERROR, "native EGL whole-target readback failed");
    std::vector<std::uint8_t> top_left(bottom_left.size());
    const auto row = static_cast<std::size_t>(size.width) * 4;
    for (unsigned y = 0; y < size.height; ++y) {
        std::copy_n(bottom_left.data() + static_cast<std::size_t>(size.height - y - 1) * row, row,
                    top_left.data() + static_cast<std::size_t>(y) * row);
    }
    return top_left;
}

void CheckUploadedPixels(GlesRenderer &renderer, RasterRenderer &commands, BufferSize size,
                         const DisplayList &list, const std::string &label)
{
    const auto uploaded = renderer.GetRenderStats().image_upload_successes;
    Require(renderer.Render(list, size.width, size.height), label + " GPU replay failed");
    Require(renderer.GetRenderStats().image_upload_successes == uploaded,
            label + " replay uploaded an already retained image");
    const auto actual = ReadNative(size);
    std::vector<std::uint8_t> expected(actual.size());
    Require(commands.Render(list, expected.data(), size.width, size.height, size.width * 4),
            label + " independent raster reference failed");
    for (std::size_t pixel = 0; pixel < expected.size(); pixel += 4) {
        std::swap(expected[pixel], expected[pixel + 2]);
    }
    CompareGpuPixels(actual, expected, size, label);
}

void CheckExplicitImageUpload(GlesRenderer &renderer, RasterRenderer &commands, BufferSize size)
{
    constexpr ResourceId upload_id{7301};
    const auto initial = renderer.GetRenderStats();
    Require(commands.RegisterImage(upload_id, Image(0)), "upload fixture registration failed");
    Require(renderer.RegisterImage(upload_id, Image(0)), "GPU source registration failed");
    Require(!renderer.ImageUploaded(upload_id), "CPU registration reported GPU residency");
    Require(!renderer.UploadImage({}), "zero ID unexpectedly uploaded");
    Require(renderer.UploadImage(upload_id), "explicit backend texture upload failed");
    Require(renderer.ImageUploaded(upload_id), "explicit texture not retained");
    auto stats = renderer.GetRenderStats();
    Require(stats.image_upload_attempts == initial.image_upload_attempts + 1 &&
                stats.image_upload_successes == initial.image_upload_successes + 1 &&
                stats.image_uploaded_bytes == initial.image_uploaded_bytes + 9 * 5 * 4,
            "explicit upload counter/decoded byte accounting mismatch");
    Require(renderer.UploadImage(upload_id), "repeat resident upload failed");
    Require(renderer.GetRenderStats().image_upload_attempts == stats.image_upload_attempts &&
                renderer.GetRenderStats().image_uploaded_bytes == stats.image_uploaded_bytes,
            "repeat resident upload did work");
    const DisplayList list{{1}, 1, {DrawImage{upload_id, {0, 0, 9, 5}, ImageFit::Fill}}};
    CheckUploadedPixels(renderer, commands, size, list, "explicit uploaded image");
    const auto first_pixels = ReadNative(size);

    Require(commands.RegisterImage(upload_id, Image(1)), "same-ID replacement failed");
    Require(renderer.ImageUploaded(upload_id),
            "raster image replacement unexpectedly mutated GPU resource table");
    Require(renderer.RegisterImage(upload_id, Image(1)), "GPU source replacement failed");
    Require(!renderer.ImageUploaded(upload_id), "same-ID replacement reused stale GPU image");
    Require(renderer.Render(list, size.width, size.height, {}), "empty replay failed");
    Require(renderer.GetRenderStats().image_upload_successes == stats.image_upload_successes,
            "empty repair uploaded an unused replacement");
    Require(renderer.UploadImage(upload_id), "replacement explicit upload failed");
    stats = renderer.GetRenderStats();
    Require(stats.image_upload_successes == initial.image_upload_successes + 2 &&
                stats.image_uploaded_bytes == initial.image_uploaded_bytes + 2 * 9 * 5 * 4,
            "replacement upload counter/byte accounting mismatch");
    CheckUploadedPixels(renderer, commands, size, list, "replacement uploaded image");
    Require(first_pixels != ReadNative(size), "replacement GPU pixels did not change");

    renderer.ReleaseImage(upload_id);
    Require(!renderer.ImageUploaded(upload_id), "ReleaseImage retained GPU residency");
    Require(renderer.UploadImage(upload_id), "released image could not reupload");
    CheckUploadedPixels(renderer, commands, size, list, "released/reuploaded image");
    commands.UnregisterImage(upload_id);
    Require(renderer.ImageUploaded(upload_id),
            "raster unregister unexpectedly mutated GPU resource table");
    renderer.UnregisterImage(upload_id);
    Require(!renderer.ImageUploaded(upload_id) && !renderer.UploadImage(upload_id),
            "unregistered image retained or uploaded a stale CPU resource");
    std::cout << "explicit GPU texture upload/replay/replacement/release bytes="
              << renderer.GetRenderStats().image_uploaded_bytes - initial.image_uploaded_bytes
              << " passed\n"
              << std::flush;
}

void RunNative(RasterRenderer &commands, const std::vector<Frame> &frames,
               const std::string &socket)
{
    using prism::platform::SubmitRequest;
    using prism::platform::SubmitResult;
    // The reference display is explicitly surfaceless; it is independent of
    // the native Wayland EGLDisplay and never supplies simulated WSI ages.
    EglDisplay reference_display;
    std::unique_ptr<GpuTarget> reference;
    prism::platform::WaylandWindow window;
    prism::platform::WaylandEglSurface egl;
    std::unique_ptr<GlesRenderer> renderer;
    BufferDamageHistory history;
    std::optional<prism::runtime::BufferDamagePlan> prepared;
    const DisplayList *committed = nullptr;
    std::uint64_t committed_resource_epoch{};
    std::uint64_t prepared_resource_epoch{};
    BufferSize reference_size{};
    BufferSize committed_reference_size{};
    std::vector<std::uint8_t> committed_full_reference, prepared_full_reference;
    const Frame *current = &frames.front();
    bool demanded = true, submitted = false;
    std::string failure;
    std::uint64_t partial_count = 0, full_count = 0, empty_count = 0, exact_count = 0, cases = 0;
    std::uint64_t forced_age_cases = 0, unknown_actual_ages = 0;
    const auto close_gpu = [&] {
        if (renderer) {
            if (!egl.MakeCurrent()) {
                renderer->Abandon();
            }
            renderer.reset();
        }
        egl.Close();
    };
    window.SetSubmitHandlers(
        [&](const SubmitRequest &request) {
            prepared.reset();
            if (!demanded && !request.force_pixels) {
                return SubmitResult::None;
            }
            if (!request.allow_pixels) {
                return SubmitResult::None;
            }
            try {
                const BufferSize size{static_cast<std::uint32_t>(request.width),
                                      static_cast<std::uint32_t>(request.height)};
                if (!egl.Ready()) {
                    Require(
                        egl.Open(request.display, request.surface, request.width, request.height),
                        "native EGL initialization failed");
                    Require(egl.GlRenderer().find("V3D") != std::string::npos,
                            "native pixel validation requires the actual V3D driver");
                    history.Reset(size);
                    std::cout << "native GL_RENDERER=" << egl.GlRenderer()
                              << " buffer_age=" << egl.Capabilities().buffer_age
                              << " partial_update=" << egl.Capabilities().partial_update
                              << " swap_damage=" << egl.Capabilities().swap_damage << '\n'
                              << std::flush;
                }
                const auto old_size = history.Size();
                const bool resized = old_size.width != size.width || old_size.height != size.height;
                Require(egl.Resize(request.width, request.height) && egl.MakeCurrent(),
                        "native target resize/current failed");
                if (resized) {
                    history.Reset(size);
                }
                if (!reference) {
                    reference = std::make_unique<GpuTarget>(reference_display, size);
                    reference_size = size;
                } else if (reference_size.width != size.width ||
                           reference_size.height != size.height) {
                    reference->Recreate(size);
                    reference_size = size;
                }
                Require(egl.MakeCurrent(), "native context restore before age query failed");
                auto content =
                    resized ? DamageRegion::Full()
                            : commands.CompareDamage(committed, current->list, request.width,
                                                     request.height, committed_resource_epoch);
                if (demanded && !resized) {
                    if (current->expected == ExpectedDamage::Full) {
                        Require(content.full, "native expected full content damage");
                    } else if (current->expected == ExpectedDamage::Empty) {
                        Require(!content.full && content.rects.empty(),
                                "native expected unchanged content");
                    } else {
                        Require(!content.full && !content.rects.empty(),
                                "native expected partial content damage");
                    }
                }
                const auto actual_age = egl.QueryBufferAge();
                if (!actual_age || !*actual_age) {
                    ++unknown_actual_ages;
                }
                std::optional<unsigned> repair_age =
                    actual_age && *actual_age >= 0
                        ? std::optional<unsigned>(static_cast<unsigned>(*actual_age))
                        : std::nullopt;
                // Explicit fault cases override the history input only. The EGL
                // age printed below remains the driver's real query result.
                if (demanded && current->age_override) {
                    repair_age = current->age_override;
                    ++forced_age_cases;
                }
                if (demanded && current->unknown_age) {
                    repair_age.reset();
                    ++forced_age_cases;
                }
                prepared = history.Plan(content, repair_age);
                if (demanded && current->full_repair) {
                    Require(prepared->repair_damage.full,
                            "native forced restoration failed to repair full target");
                }
                Require(egl.SetDamage(prepared->repair_damage) !=
                            prism::platform::DamageRegionResult::Failed,
                        "native repair declaration failed");
                const auto before_native = ReadNative(size);
                if (!renderer) {
                    renderer = std::make_unique<GlesRenderer>(default_font_path);
                    Require(renderer->RegisterFont(serif_font, serif_font_path) &&
                                renderer->RegisterImage(image_id, Image(0)),
                            "native GPU resource registration failed");
                    CheckExplicitImageUpload(*renderer, commands, size);
                }
                Require(renderer->Ready() &&
                            renderer->Render(current->list, request.width, request.height,
                                             prepared->repair_damage),
                        "native partial render failed");
                const auto actual = ReadNative(size);
                CompareOutside(before_native, actual, size, prepared->repair_damage,
                               "native frame=" + current->name + " repair preservation");
                reference->Current();
                Require(reference->Renderer().Render(current->list, request.width, request.height),
                        "independent full reference failed");
                prepared_full_reference = reference->Read(size);
                if (!committed_full_reference.empty() &&
                    committed_reference_size.width == size.width &&
                    committed_reference_size.height == size.height) {
                    CompareOutside(committed_full_reference, prepared_full_reference, size, content,
                                   "native frame=" + current->name +
                                       " independent full-reference content coverage");
                }
                CompareGpuPixels(actual, prepared_full_reference, size,
                                 "native frame=" + current->name);
                // Full reference readback left another context current. The Swap
                // and next native draw must use this window's own context again.
                Require(egl.MakeCurrent(), "native context restore before Swap failed");
                prepared_resource_epoch = commands.ResourceEpoch();
                const auto area = prism::runtime::DamageArea(prepared->repair_damage, size);
                std::cout << "native validated frame=" << current->name
                          << " actual_age=" << actual_age.value_or(-1) << " history_age="
                          << (repair_age ? std::to_string(*repair_age) : "unknown") << " repair="
                          << (prepared->repair_damage.full            ? "full"
                              : prepared->repair_damage.rects.empty() ? "empty"
                                                                      : "partial")
                          << " repair_pixels=" << area << " target=" << size.width << 'x'
                          << size.height << '\n'
                          << std::flush;
                ++exact_count;
                return SubmitResult::Pixels;
            } catch (const std::exception &error) {
                failure = error.what();
                close_gpu();
                return SubmitResult::Failed;
            }
        },
        [&] {
            if (!prepared || !egl.Swap(prepared->content_damage)) {
                failure = "native Swap failed";
                close_gpu();
                return false;
            }
            const bool full = prepared->repair_damage.full,
                       empty = prepared->repair_damage.rects.empty();
            if (!history.Commit(std::move(*prepared))) {
                failure = "native successful Swap could not publish history";
                close_gpu();
                return false;
            }
            // Plain pointers refer to immutable, preallocated frame storage. No
            // snapshot allocation occurs after the successful posting operation.
            committed = &current->list;
            committed_resource_epoch = prepared_resource_epoch;
            committed_full_reference = std::move(prepared_full_reference);
            committed_reference_size = history.Size();
            prepared.reset();
            if (full) {
                ++full_count;
            } else if (empty) {
                ++empty_count;
            } else {
                ++partial_count;
            }
            submitted = true;
            demanded = false;
            return true;
        },
        [&](SubmitResult result) {
            if (result == SubmitResult::Failed) {
                close_gpu();
            }
        });
    Require(commands.RegisterImage(image_id, Image(0)), "native initial image registration failed");
    Require(window.Open(socket, "org.prism.validation.damage.native",
                        "Native damage pixel verification", initial_size.width,
                        initial_size.height),
            "native Wayland window open failed");
    const auto wait = [&](const std::function<bool()> &condition, const char *detail) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!condition()) {
            Require(std::chrono::steady_clock::now() < deadline, detail);
            Require(window.Pump(10), failure.empty() ? "native Wayland client stopped" : failure);
        }
    };
    const auto drain = [&] {
        wait([&] { return !window.FrameCallbackPending(); },
             "native frame callback did not complete");
        for (int i = 0; i < 4; ++i) {
            Require(window.Pump(0), "native residual configure drain failed");
        }
        wait([&] { return !window.FrameCallbackPending(); },
             "native resize callback did not complete");
    };
    for (const auto &frame : frames) {
        // BSP owns real toplevel geometry. The separate SDK native probe tests
        // real peer-driven resize; this test does not invent configure sizes.
        if (frame.resize) {
            std::cout << "native skip explicit synthetic resize case\n";
            continue;
        }
        if (frame.image_variant) {
            Require(committed && committed->commands == frame.list.commands,
                    "native image replacement altered display commands");
            Require(commands.RegisterImage(image_id, Image(*frame.image_variant)),
                    "native image replacement failed");
            Require(egl.MakeCurrent() &&
                        renderer->RegisterImage(image_id, Image(*frame.image_variant)),
                    "native GPU image replacement failed");
            reference->SetImage(*frame.image_variant);
        }
        if (frame.reset_epoch) {
            history.Reset(history.Size());
        }
        current = &frame;
        demanded = true;
        submitted = false;
        window.RequestUpdate(true);
        wait([&] { return submitted; }, "native case did not Swap");
        drain();
        ++cases;
    }
    Require(history.SuccessfulSequence() == window.GetSubmitStats().pixel_commits,
            "native history advanced without a successful pixel submission");
    const bool age_supported = egl.Capabilities().buffer_age;
    if (age_supported) {
        Require(partial_count > 0, "real EGL ages did not exercise partial native repair");
    }
    std::cout << "native entire-target validated cases=" << cases << " readbacks=" << exact_count
              << " partial=" << partial_count << " full=" << full_count << " empty=" << empty_count
              << " unknown_actual_ages=" << unknown_actual_ages
              << " diagnostic_age_overrides=" << forced_age_cases
              << " buffer_age_supported=" << age_supported
              << (age_supported ? "" : " (capability fallback: no native partial-age proof)")
              << '\n'
              << std::flush;
    close_gpu();
    window.Close();
}
} // namespace

int main(int argc, char **argv)
{
    RasterRenderer commands(default_font_path);
    TextShaper shaper(default_font_path);
    Require(commands.Ready() && shaper.Ready(), "default font unavailable");
    Require(commands.RegisterFont(serif_font, serif_font_path), "alternate font unavailable");
    Require(shaper.RegisterFont(serif_font, serif_font_path), "alternate shaping font unavailable");
    const auto frames = Frames(shaper);
    if (argc == 3 && std::string_view(argv[1]) == "--wayland") {
        RunNative(commands, frames, argv[2]);
        return 0;
    }
    Require(argc == 1, "usage: skia_damage_test [--wayland <socket>]");
    EglDisplay display;
    {
        GpuTarget target(display, initial_size);
        target.Current();
        CheckExplicitImageUpload(target.Renderer(), commands, initial_size);
    }
    CheckLargeRepairDoesNotExpand(display, commands);
    for (unsigned buffer_count : {1u, 2u, 3u}) {
        RunRotation(display, commands, frames, buffer_count);
    }
}
