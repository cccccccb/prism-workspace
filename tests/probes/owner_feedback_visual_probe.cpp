#include "prism/contracts/display_list_validation.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/owner_feedback_panel.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/theme/compiler.hpp"

#include <png.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace prism;

namespace {
constexpr std::string_view font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";

struct Case {
    std::string_view name;
    std::string_view scheme;
    int width;
    int height;
};

constexpr std::array cases{
    Case{"dark-640x420", "dark", 640, 420}, Case{"light-640x420", "light", 640, 420},
    Case{"narrow-240x180", "dark", 240, 180}, Case{"short-640x204", "dark", 640, 204}};

std::string Read(const std::filesystem::path &path)
{
    std::ifstream input(path);
    assert(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}

runtime::Blueprint Context()
{
    // Deliberately a small Notes-style context, not the actual Notepad program.
    return runtime::ParseBlueprint(R"(
Card(background: "@surfaceRaised", padding: "@window_padding") {
    VStack(spacing: 8) {
        HStack(height: 32, spacing: 8, align: "center") {
            Icon("document", width: 20, height: 20, foreground: "@accent")
            Text("Notes", flex: 1, font: "@font_title", foreground: "@text")
            Text("Unsaved", font: "@font_body", foreground: "@mutedText")
        }
        Separator(height: 1, background: "@controlOutline")
        Card(flex: 1, padding: 12, background: "@controlSecondary", cornerRadius: "@control_state_radius", clip: true) {
            VStack(spacing: 8) {
                Text("Prism notes.txt", height: 22, font: "@font_body", foreground: "@text")
                Text("Keep the working context.\nYour draft stays here while you\nchoose how to recover.", font: "@font_body", lineHeight: "@line_body", foreground: "@mutedText")
                Card(flex: 1)
            }
        }
    }
})");
}

runtime::TextLayoutInfo Layout(const runtime::Scene &scene, std::string_view region)
{
    const auto metrics = scene.TextLayoutInRegion(region);
    assert(metrics && std::isfinite(metrics->width) && metrics->width > 0 &&
           std::isfinite(metrics->height) && metrics->height > 0 && metrics->font_size > 0);
    return *metrics;
}

std::string Wrap(std::string_view text, const runtime::TextLayoutInfo &layout,
                 const runtime::ShapeText &shape)
{
    const auto result = runtime::WrapOwnerTaskText(text, layout.width, layout.font_size, shape);
    std::size_t start = 0;
    while (start < result.size()) {
        const auto end = result.find('\n', start);
        const auto line = std::string_view(result).substr(
            start, end == std::string::npos ? result.size() - start : end - start);
        const auto metrics = shape(line, layout.font_size);
        assert(std::isfinite(metrics.width) && std::isfinite(metrics.height));
        assert(metrics.width <= layout.width + 0.001 && metrics.height <= layout.height + 0.001);
        if (!line.empty()) {
            assert(!metrics.glyphs.empty() && metrics.width > 0 && metrics.height > 0);
            for (const auto &glyph : metrics.glyphs) {
                assert(glyph.glyph_index != 0);
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

void CheckGeometry(runtime::Scene &scene, const contracts::OwnerFeedbackRequest &request,
                   const runtime::ShapeText &shape)
{
    const auto input = scene.CaptureInputSnapshot();
    const auto card = input->Find(scene.RegionId(runtime::kOwnerFeedbackCardRegion));
    assert(card && card->visible && card->bounds.width <= 336 && card->bounds.height <= 148);
    assert(card->bounds.x >= 0 && card->bounds.y >= 0);
    assert(card->bounds.x + card->bounds.width <= input->viewport.width);
    assert(card->bounds.y + card->bounds.height <= input->viewport.height);
    for (const auto &node : input->nodes) {
        if (!node.visible || !runtime::IsOwnerFeedbackReservedName(node.action)) {
            continue;
        }
        assert(node.interactive && node.bounds.width >= 32 && node.bounds.height >= 32);
        assert(node.bounds.x >= card->bounds.x && node.bounds.y >= card->bounds.y);
        assert(node.bounds.x + node.bounds.width <= card->bounds.x + card->bounds.width);
        assert(node.bounds.y + node.bounds.height <= card->bounds.y + card->bounds.height);
    }
    for (std::size_t index = 0; index < request.actions.size(); ++index) {
        const auto label = Layout(scene, runtime::kOwnerFeedbackLabelRegions[index]);
        const auto metrics = shape(request.actions[index].label, label.font_size);
        assert(!metrics.glyphs.empty() && metrics.width > 0 && metrics.height > 0);
        assert(metrics.width <= label.width && metrics.height <= label.height);
    }
}

void WritePng(const std::filesystem::path &path, const std::vector<std::uint8_t> &bgra, int width,
              int height)
{
    // Skia's CPU buffer is premultiplied BGRA. Export straight RGBA for PNG.
    std::vector<std::uint8_t> rgba(bgra.size());
    for (std::size_t index = 0; index < bgra.size(); index += 4) {
        const auto alpha = static_cast<unsigned>(bgra[index + 3]);
        rgba[index + 3] = static_cast<std::uint8_t>(alpha);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto value = static_cast<unsigned>(bgra[index + 2 - channel]);
            rgba[index + channel] = static_cast<std::uint8_t>(
                alpha ? std::min(255u, (value * 255u + alpha / 2) / alpha) : 0);
        }
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(width);
    image.height = static_cast<png_uint_32>(height);
    image.format = PNG_FORMAT_RGBA;
    const auto target = path.string();
    const auto written =
        png_image_write_to_file(&image, target.c_str(), 0, rgba.data(), 0, nullptr);
    if (!written) {
        std::cerr << image.message << '\n';
    }
    assert(written);
    png_image_free(&image);
}

void RenderCase(const std::filesystem::path &root, const std::filesystem::path &output,
                const runtime::Blueprint &shared, const Case &test)
{
    runtime::TextShaper shaper{std::string(font)};
    render_skia::RasterRenderer renderer{std::string(font)};
    assert(shaper.Ready() && renderer.Ready());
    const runtime::ShapeText shape = std::bind_front(
        static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
            &runtime::TextShaper::Shape),
        &shaper);
    const auto theme = theme::LoadTheme(root / "resources/themes", "glass", 1, test.scheme);
    runtime::Scene scene(runtime::ComposeOwnerFeedbackPanel(Context(), shared), shape,
                         shaper.FontId(), theme);
    scene.SetViewport({static_cast<double>(test.width), static_cast<double>(test.height)});
    auto values = runtime::OwnerFeedbackPanelDefaults();
    assert(scene.PrepareDetached(values));
    assert(!scene.IsVisible(scene.RegionId(runtime::kOwnerFeedbackPanelRegion)));

    const contracts::OwnerFeedbackRequest request{
        19,
        contracts::OwnerFeedbackKind::Error,
        "Could not save",
        "The destination is unavailable.\nYour draft is retained. Choose another folder.",
        {{1, "Change location"}},
        0};
    assert(contracts::ValidateOwnerFeedbackRequest(request));
    values = runtime::OwnerFeedbackPanelBindings(request);
    const std::array updates{
        runtime::RegionUpdate{std::string(runtime::kOwnerFeedbackPanelRegion),
                              runtime::InstantiateOwnerFeedbackPanel(shared, request, 1)}};
    std::string diagnostic;
    assert(scene.ReplaceRegions(updates, values, &diagnostic));
    assert(diagnostic.empty());
    scene.ResolveLayout();
    values.insert_or_assign(
        "__prism_feedback_title",
        Wrap(request.title, Layout(scene, runtime::kOwnerFeedbackTitleRegion), shape));
    values.insert_or_assign(
        "__prism_feedback_message",
        Wrap(request.message, Layout(scene, runtime::kOwnerFeedbackMessageRegion), shape));
    assert(scene.Preflight(values, &diagnostic));
    assert(diagnostic.empty());
    const auto list = scene.Build({1});
    assert(list);
    contracts::ValidateDisplayList(*list);
    CheckGeometry(scene, request, shape);
    std::size_t glyph_runs = 0;
    for (const auto &command : list->commands) {
        if (const auto *run = std::get_if<contracts::DrawGlyphRun>(&command)) {
            assert(!run->glyphs.empty() && run->font == shaper.FontId());
            ++glyph_runs;
        }
    }
    assert(glyph_runs >= 5);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(test.width) * test.height * 4);
    assert(renderer.Render(*list, pixels.data(), test.width, test.height, test.width * 4));
    const auto path = output / (std::string(test.name) + ".png");
    WritePng(path, pixels, test.width, test.height);
    std::cout << path
              << " — CPU raster, real DejaVu Sans; shared feedback in Notes-style context\n";
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 3);
    const std::filesystem::path root{argv[1]}, output{argv[2]};
    std::filesystem::create_directories(output);
    const auto shared = runtime::LinkComponent(
        runtime::PrepareComponent(Read(root / "resources/ui/owner-feedback-panel.prism")));
    for (const auto &test : cases) {
        RenderCase(root, output, shared, test);
    }
}
