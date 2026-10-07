#include "prism/render_skia/gles_renderer.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace prism::contracts;
using prism::render_skia::GlesRenderer;
constexpr auto font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
constexpr std::uint64_t context_id = 17;
constexpr ResourceId image_id{91};
constexpr int width = 32, height = 24;

[[noreturn]] void Fail(const std::string &detail)
{
    std::cerr << "skia_multi_target_test: " << detail << '\n';
    std::abort();
}

void Require(bool value, const std::string &detail)
{
    if (!value) {
        Fail(detail);
    }
}

struct Target {
    EGLSurface native{EGL_NO_SURFACE};
    GpuTargetIdentity identity{};
};

class EglFixture {
public:
    EglFixture()
    {
        auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        Require(get_display, "surfaceless display entry point missing");
        display_ = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        Require(display_ != EGL_NO_DISPLAY && eglInitialize(display_, nullptr, nullptr),
                "surfaceless display initialization failed");
        Require(eglBindAPI(EGL_OPENGL_ES_API), "GLES API unavailable");

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
                                     EGL_NONE};
        EGLint count = 0;
        Require(eglChooseConfig(display_, attributes, &config_, 1, &count) && count == 1,
                "GLES3 RGBA pbuffer config missing");
        context_ = CreateContext();
        foreign_ = CreateContext();
    }

    ~EglFixture()
    {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        for (auto surface : surfaces_) {
            if (surface != EGL_NO_SURFACE) {
                eglDestroySurface(display_, surface);
            }
        }
        eglDestroyContext(display_, foreign_);
        eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }

    Target Create(int target_width = width, int target_height = height)
    {
        const EGLint attributes[] = {EGL_WIDTH, target_width, EGL_HEIGHT, target_height, EGL_NONE};
        const auto surface = eglCreatePbufferSurface(display_, config_, attributes);
        Require(surface != EGL_NO_SURFACE, "pbuffer creation failed");
        surfaces_.push_back(surface);
        return {surface, NextIdentity()};
    }

    GpuTargetIdentity NextIdentity()
    {
        return {context_id, next_surface_++, 1};
    }

    void Current(const Target &target)
    {
        Require(eglMakeCurrent(display_, target.native, target.native, context_),
                "main context target switch failed");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        GLint framebuffer = -1;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        Require(framebuffer == 0 && glGetError() == GL_NO_ERROR,
                "test target is not default FBO 0");
    }

    void ForeignCurrent(const Target &target)
    {
        Require(eglMakeCurrent(display_, target.native, target.native, foreign_),
                "foreign context switch failed");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void Destroy(Target &target)
    {
        Require(eglGetCurrentSurface(EGL_DRAW) != target.native &&
                    eglGetCurrentSurface(EGL_READ) != target.native,
                "test must switch away before destroying a pbuffer");
        const auto found = std::find(surfaces_.begin(), surfaces_.end(), target.native);
        Require(found != surfaces_.end() && eglDestroySurface(display_, target.native),
                "pbuffer destruction failed");
        *found = EGL_NO_SURFACE;
        target = {};
    }

private:
    EGLContext CreateContext()
    {
        const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        const auto context = eglCreateContext(display_, config_, EGL_NO_CONTEXT, attributes);
        Require(context != EGL_NO_CONTEXT, "GLES context creation failed");
        return context;
    }

    EGLDisplay display_{EGL_NO_DISPLAY};
    EGLConfig config_{};
    EGLContext context_{EGL_NO_CONTEXT}, foreign_{EGL_NO_CONTEXT};
    std::uint64_t next_surface_{1};
    std::vector<EGLSurface> surfaces_;
};

struct Pixels {
    int width{}, height{};
    std::vector<std::uint8_t> rgba;

    std::array<std::uint8_t, 4> At(int x, int y) const
    {
        const auto offset = (static_cast<std::size_t>(height - y - 1) * width + x) * 4;
        return {rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]};
    }

    bool operator==(const Pixels &) const = default;
};

Pixels Read(int target_width = width, int target_height = height)
{
    Pixels result{target_width, target_height,
                  std::vector<std::uint8_t>(target_width * target_height * 4)};
    glReadPixels(0, 0, target_width, target_height, GL_RGBA, GL_UNSIGNED_BYTE, result.rgba.data());
    Require(glGetError() == GL_NO_ERROR, "GPU pixel readback failed");
    return result;
}

void Expect(const Pixels &pixels, int x, int y, Color color, const std::string &detail)
{
    const std::array<std::uint8_t, 4> expected{color.r, color.g, color.b, color.a};
    Require(pixels.At(x, y) == expected, detail);
}

prism::runtime::DecodedImage Image()
{
    return {1, 1, {239, 181, 31, 255}};
}

DisplayList Frame(Color background, Color patch)
{
    return {{1},
            1,
            {FillRect{{0, 0, width, height}, background},
             FillRect{{2, 1, 5, 2}, {30, 230, 100, 255}},
             FillRect{{25, 20, 5, 2}, {200, 20, 250, 255}}, FillRect{{8, 7, 8, 6}, patch},
             DrawImage{image_id, {20, 3, 6, 6}, ImageFit::Fill}}};
}

void OutsideUnchanged(const Pixels &before, const Pixels &after)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (x >= 8 && x < 16 && y >= 7 && y < 13) {
                continue;
            }
            Require(before.At(x, y) == after.At(x, y), "repair escaped its target rectangle");
        }
    }
}

void CheckAlternatingTargets(EglFixture &fixture)
{
    auto first = fixture.Create();
    const auto second = fixture.Create();
    fixture.Current(first);
    const auto *vendor = glGetString(GL_VENDOR);
    const auto *renderer_name = glGetString(GL_RENDERER);
    Require(vendor && renderer_name, "GPU identity missing");
    std::cout << "surfaceless pbuffer GLES vendor=" << vendor << " renderer=" << renderer_name
              << '\n';

    GlesRenderer renderer(font_path, first.identity);
    Require(renderer.Ready() && renderer.RegisterImage(image_id, Image()) &&
                renderer.UploadImage(image_id),
            "typed renderer/image creation failed");
    constexpr Color first_background{210, 40, 70, 255};
    constexpr Color second_background{40, 70, 210, 255};
    constexpr Color initial_patch{80, 100, 120, 255};
    auto first_frame = Frame(first_background, initial_patch);
    auto second_frame = Frame(second_background, initial_patch);
    Require(renderer.Render(first_frame, first.identity, width, height), "first target failed");
    auto first_pixels = Read();
    Expect(first_pixels, 3, 1, {30, 230, 100, 255}, "first target upper marker misplaced");
    Expect(first_pixels, 27, 20, {200, 20, 250, 255}, "first target lower marker misplaced");
    Expect(first_pixels, 23, 6, {239, 181, 31, 255}, "uploaded image missing on first target");

    fixture.Current(second);
    Require(renderer.Render(second_frame, second.identity, width, height), "second target failed");
    auto second_pixels = Read();
    Expect(second_pixels, 1, 12, second_background, "second target background incorrect");
    Expect(second_pixels, 23, 6, {239, 181, 31, 255}, "shared image missing on second target");
    const auto initial_stats = renderer.GetRenderStats();
    Require(initial_stats.target_wraps == 2 && initial_stats.image_upload_successes == 1,
            "each target needs a wrapper while the image needs only one upload");

    const DamageRegion repair{false, {{8, 7, 8, 6}}};
    for (unsigned step = 0; step < 24; ++step) {
        const Color first_patch{static_cast<std::uint8_t>(90 + step), 220, 130, 255};
        const Color second_patch{230, static_cast<std::uint8_t>(90 + step), 60, 255};
        first_frame = Frame(first_background, first_patch);
        second_frame = Frame(second_background, second_patch);

        fixture.Current(first);
        Require(renderer.Render(first_frame, first.identity, width, height, repair),
                "first target partial repair failed");
        const auto repaired_first = Read();
        OutsideUnchanged(first_pixels, repaired_first);
        Expect(repaired_first, 10, 9, first_patch, "first target patch color incorrect");
        first_pixels = repaired_first;

        fixture.Current(second);
        Require(Read() == second_pixels, "drawing first target changed second target");
        Require(renderer.Render(second_frame, second.identity, width, height, repair),
                "second target partial repair failed");
        const auto repaired_second = Read();
        OutsideUnchanged(second_pixels, repaired_second);
        Expect(repaired_second, 10, 9, second_patch, "second target patch color incorrect");
        second_pixels = repaired_second;
        fixture.Current(first);
        Require(Read() == first_pixels, "drawing second target changed first target");
    }
    const auto repeated = renderer.GetRenderStats();
    Require(repeated.target_wraps == 2 && repeated.target_cache_hits >= 48 &&
                repeated.partial_renders == 48 && repeated.image_upload_successes == 1,
            "target rotation rebuilt wrappers or repeated image upload");

    fixture.Current(second);
    const auto before_rejection = Read();
    Require(!renderer.Render(first_frame, first.identity, width, height),
            "same identity accepted another native surface");
    auto foreign_id = second.identity;
    ++foreign_id.context_lifetime_id;
    Require(!renderer.Render(second_frame, foreign_id, width, height) &&
                !renderer.Render(second_frame, {}, width, height) &&
                !renderer.Render(second_frame, second.identity, width / 2, height) &&
                !renderer.Render(second_frame, width, height),
            "typed target accepted foreign/empty/unchanged-generation/legacy identity");
    Require(Read() == before_rejection, "identity rejection wrote target pixels");

    const auto first_old_id = first.identity;
    Require(renderer.ReleaseTarget(first.identity), "inactive first target release failed");
    fixture.Destroy(first);
    first = fixture.Create();
    Require(first.identity != first_old_id, "recreated surface reused its identity");
    fixture.Current(first);
    Require(renderer.Render(first_frame, first.identity, width, height),
            "recreated same-size target failed");
    Expect(Read(), 1, 12, first_background, "recreated target retained another target's pixels");
    Require(renderer.GetRenderStats().image_upload_successes == 1,
            "target recreation reuploaded a context-owned image");
    Require(renderer.ReleaseTarget(first.identity) && renderer.ReleaseTarget(second.identity),
            "target cleanup failed");
    renderer.Close();
    Require(!renderer.Ready(), "closed renderer retained context resources");
    std::cout << "two same-size FBO0 targets: 48 exact partial repairs; independent asymmetric "
                 "markers; 2 cached wrappers; image uploaded once; recreated identity isolated\n";
}

class FramebufferTarget {
public:
    explicit FramebufferTarget(GpuTargetIdentity target) : identity(target)
    {
        glGenFramebuffers(1, &framebuffer_);
        glGenTextures(1, &texture_);
        Resize(24, 16);
    }

    ~FramebufferTarget()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer_);
        glDeleteTextures(1, &texture_);
    }

    void Resize(int next_width, int next_height)
    {
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, next_width, next_height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                    glGetError() == GL_NO_ERROR,
                "resized framebuffer storage incomplete");
        if (target_width) {
            ++identity.resize_generation;
        }
        target_width = next_width;
        target_height = next_height;
    }

    void Bind()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    }

    GpuTargetIdentity identity;
    int target_width{}, target_height{};

private:
    GLuint framebuffer_{}, texture_{};
};

DisplayList Solid(int target_width, int target_height, Color color)
{
    return {{1}, 1, {FillRect{{0, 0, double(target_width), double(target_height)}, color}}};
}

void CheckSameTargetBindingRestore(EglFixture &fixture)
{
    const auto native = fixture.Create();
    fixture.Current(native);
    FramebufferTarget scratch(fixture.NextIdentity());
    const auto scratch_pixels = Read(scratch.target_width, scratch.target_height);
    fixture.Current(native);
    GlesRenderer renderer(font_path, native.identity);
    Require(renderer.Ready(), "binding restoration renderer unavailable");

    DisplayList frame{{1},
                      1,
                      {FillRect{{0, 0, width, height}, {40, 80, 120, 255}},
                       FillRect{{8, 7, 8, 6}, {60, 100, 140, 255}}, PushOpacity{0.5},
                       FillRoundedRect{{19, 5, 10, 14}, 3, {220, 90, 160, 255}},
                       FillRect{{22, 8, 5, 8}, {50, 210, 170, 255}}, PopOpacity{}}};
    Require(renderer.Render(frame, native.identity, width, height), "scratch-layer frame failed");
    const auto before = Read();
    scratch.Bind();
    Require(Read(scratch.target_width, scratch.target_height) == scratch_pixels,
            "initial root draw changed external framebuffer");
    const auto wraps = renderer.GetRenderStats().target_wraps;
    Require(!renderer.Render(frame, native.identity, width, height),
            "same-generation identity accepted a different bound framebuffer");

    fixture.Current(native); // Models WSI rebinding FBO 0 on the same target.
    std::get<FillRect>(frame.commands[1]).color = {90, 220, 130, 255};
    const DamageRegion repair{false, {{8, 7, 8, 6}}};
    Require(renderer.Render(frame, native.identity, width, height, repair),
            "same target could not resume after external bind/unbind");
    const auto after = Read();
    OutsideUnchanged(before, after);
    Expect(after, 10, 9, {90, 220, 130, 255}, "restored target repair missed default framebuffer");
    Require(renderer.GetRenderStats().target_wraps == wraps,
            "same-target framebuffer restoration rebuilt its wrapper");
    scratch.Bind();
    Require(Read(scratch.target_width, scratch.target_height) == scratch_pixels,
            "default framebuffer repair wrote external framebuffer");
    fixture.Current(native);
    Require(renderer.ReleaseTarget(native.identity), "binding restoration cleanup failed");
    renderer.Close();
    std::cout << "same target: external framebuffer bind/unbind and opacity scratch layer "
                 "restore FBO0 repair with one cached wrapper\n";
}

void CheckStorageGenerations(EglFixture &fixture)
{
    const auto native = fixture.Create(64, 48);
    fixture.Current(native);
    GlesRenderer renderer(font_path, native.identity);
    Require(renderer.Ready(), "generation renderer unavailable");
    FramebufferTarget target(fixture.NextIdentity());
    const auto oldest = target.identity;
    const auto first = Solid(24, 16, {201, 41, 81, 255});
    Require(renderer.Render(first, target.identity, 24, 16), "initial FBO storage render failed");
    Expect(Read(24, 16), 12, 8, {201, 41, 81, 255}, "initial FBO pixels incorrect");

    for (unsigned step = 0; step < 70; ++step) {
        const int target_width = step % 2 ? 24 : 40;
        const int target_height = step % 2 ? 16 : 28;
        const auto stale = target.identity;
        target.Resize(target_width, target_height);
        const Color color{static_cast<std::uint8_t>(30 + step), 173, 220, 255};
        const auto frame = Solid(target_width, target_height, color);
        Require(renderer.Render(frame, target.identity, target_width, target_height),
                "new storage generation failed or accumulated wrapper slots");
        const auto pixels = Read(target_width, target_height);
        Expect(pixels, target_width - 1, target_height - 1, color,
               "new storage dimensions not represented by wrapper");
        Require(!renderer.Render(frame, stale, target_width, target_height) &&
                    !renderer.ReleaseTarget(stale),
                "stale generation rendered or released its replacement");
        Require(Read(target_width, target_height) == pixels,
                "stale generation modified current storage");
    }
    Require(!renderer.Render(first, oldest, 24, 16), "oldest generation was resurrected");
    Require(renderer.GetRenderStats().target_wraps == 71 && renderer.ReleaseTarget(target.identity),
            "storage generation cleanup failed");
    renderer.Close();
    std::cout << "framebuffer storage: 70 real resize generations replace one live target; "
                 "stale generation cannot draw or release\n";
}

void CheckTargetBudget(EglFixture &fixture)
{
    std::vector<Target> targets;
    for (unsigned index = 0; index < 65; ++index) {
        targets.push_back(fixture.Create(4, 4));
    }
    fixture.Current(targets.front());
    GlesRenderer renderer(font_path, targets.front().identity);
    const auto frame = Solid(4, 4, {50, 150, 220, 255});
    for (unsigned index = 0; index < 64; ++index) {
        fixture.Current(targets[index]);
        Require(renderer.Render(frame, targets[index].identity, 4, 4),
                "valid live target below cache budget rejected");
        Expect(Read(4, 4), 2, 2, {50, 150, 220, 255}, "budget target pixels incorrect");
    }
    fixture.Current(targets.back());
    Require(!renderer.Render(frame, targets.back().identity, 4, 4) &&
                renderer.GetRenderStats().target_wraps == 64,
            "65th live target exceeded wrapper budget");
    Require(renderer.ReleaseTarget(targets.front().identity), "budget slot release failed");
    Require(renderer.Render(frame, targets.back().identity, 4, 4),
            "released budget slot was not reusable by a new lifetime");
    for (unsigned index = 1; index < 65; ++index) {
        Require(renderer.ReleaseTarget(targets[index].identity), "budget target cleanup failed");
    }
    renderer.Close();
    std::cout << "live-target budget: 64 real FBO0 surfaces admitted; 65th rejected; released "
                 "slot recovers\n";
}

void CheckLegacyAndForeignContext(EglFixture &fixture)
{
    const auto first = fixture.Create();
    const auto second = fixture.Create();
    const auto frame = Solid(width, height, {120, 190, 30, 255});
    fixture.Current(first);
    GlesRenderer legacy(font_path);
    Require(legacy.Ready() && legacy.Render(frame, width, height), "legacy target rejected");
    fixture.Current(second);
    const auto untouched = Read();
    Require(!legacy.Render(frame, width, height) && Read() == untouched,
            "legacy API allowed target switch");
    fixture.Current(first);
    Require(legacy.Render(frame, width, height), "legacy original target could not resume");
    legacy.Close();

    GlesRenderer typed(font_path, first.identity);
    Require(typed.Ready() && typed.RegisterImage(image_id, Image()) &&
                typed.Render(Frame({20, 80, 120, 255}, {30, 90, 140, 255}), first.identity, width,
                             height),
            "foreign-context source renderer setup failed");
    fixture.ForeignCurrent(second);
    std::array<GLuint, 64> sentinels{};
    glGenTextures(sentinels.size(), sentinels.data());
    for (auto texture : sentinels) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    Require(glGetError() == GL_NO_ERROR, "foreign texture sentinel creation failed");
    const auto foreign_pixels = Read();
    Require(!typed.Render(frame, first.identity, width, height) && !typed.UploadImage(image_id) &&
                !typed.ReleaseTarget(first.identity),
            "renderer used or deleted target with a foreign current context");
    Require(Read() == foreign_pixels, "foreign context pixels modified");
    typed.Close();
    Require(!typed.Ready(), "foreign-current Close did not abandon renderer");
    for (auto texture : sentinels) {
        Require(glIsTexture(texture), "abandon deleted another context's numbered texture");
    }
    glDeleteTextures(sentinels.size(), sentinels.data());
    fixture.Current(first);

    GlesRenderer invalid(font_path, GpuTargetIdentity{});
    Require(!invalid.Ready(), "invalid typed constructor created a live renderer");
    std::cout << "legacy target locked; foreign context rejects rendering/upload/release; "
                 "abandon preserves all 64 foreign GL texture sentinels\n";
}
} // namespace

int main()
{
    EglFixture fixture;
    CheckAlternatingTargets(fixture);
    CheckSameTargetBindingRestore(fixture);
    CheckStorageGenerations(fixture);
    CheckTargetBudget(fixture);
    CheckLegacyAndForeignContext(fixture);
    std::cout << "Skia multi-target gates passed (surfaceless/pbuffer; no Wayland WSI claim)\n";
}
