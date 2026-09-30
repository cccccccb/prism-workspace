#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include <cassert>
#include <stdexcept>
#include <variant>

using namespace prism;

namespace {
std::optional<prism::contracts::DisplayList> BuildAndCommit(prism::runtime::Scene &scene)
{
    auto list = scene.Build(prism::contracts::WindowId{1});
    scene.AcknowledgeComposite();
    return list;
}
} // namespace

int main()
{
    int shape_calls = 0;
    auto shape = [&shape_calls](std::string_view text, double size) {
        ++shape_calls;
        runtime::ShapedText result;
        result.width = text.size() * size * 0.5;
        result.height = size * 1.4;
        for (std::size_t i = 0; i < text.size(); ++i) {
            result.glyphs.push_back(
                {static_cast<std::uint32_t>(static_cast<unsigned char>(text[i])),
                 {i * size * 0.5, size}});
        }
        return result;
    };
    auto blueprint = runtime::ParseBlueprint(
        "HStack(spacing: 8) { Button(\"Launch\", action: \"app:launch\", width: 100) "
        "VStack { Text($title, font: 20) Card(width: 40, height: 30).cornerRadius(6) } }");
    blueprint.properties.push_back(
        {runtime::DslProperty::Background, contracts::Color{12, 24, 36, 255}});
    blueprint.properties.push_back({runtime::DslProperty::Clip, true});
    runtime::Scene scene(std::move(blueprint), shape, contracts::ResourceId{7});
    assert(scene.SetViewport({300, 120}));
    auto first = BuildAndCommit(scene);
    assert(first && first->generation == 1);
    assert(!BuildAndCommit(scene)); // idle pages do not submit
    assert(scene.ActionAt({20, 20}) == "app:launch");
    assert(!scene.ActionAt({180, 20}));
    assert(std::holds_alternative<contracts::PushClipRect>(first->commands.front()));
    assert(std::holds_alternative<contracts::PopClip>(first->commands.back()));
    assert(scene.SetSlot("title", "Prism"));
    assert(runtime::Has(scene.PendingDirty(), runtime::Dirty::Layout));
    auto second = BuildAndCommit(scene);
    assert(second && second->generation == 2);
    bool found = false;
    for (const auto &command : second->commands) {
        if (auto *run = std::get_if<contracts::DrawGlyphRun>(&command)) {
            if (run->glyphs.size() == 5 && run->font.value == 7) {
                found = true;
            }
        }
    }
    assert(found);
    assert(!scene.SetSlot("title", "Prism"));
    assert(!BuildAndCommit(scene));
    assert(scene.SetBackground(scene.RootId(), {20, 30, 40, 255}));
    assert(scene.PendingDirty() == runtime::Dirty::Paint);
    const int before_paint = shape_calls;
    assert(BuildAndCommit(scene));
    assert(shape_calls == before_paint);
    assert(!scene.SetViewport({0, 120}));
    assert(!BuildAndCommit(scene));
    assert(scene.Bounds({999, 1}).width == 0);
    runtime::Scene bound(
        runtime::ParseBlueprint("Card(background: $surface) { Text($label, font: $size) }"), shape,
        contracts::ResourceId{7});
    assert(bound.SetViewport({120, 80}));
    assert(BuildAndCommit(bound));
    assert(!bound.SetBackground(bound.RootId(), contracts::Color{0, 0, 0, 0}));
    assert(!bound.AcceptsBinding("missing", std::string("value")));
    assert(!bound.AcceptsBinding("surface", std::string("wrong type")));
    assert(!bound.SetBinding("surface", std::string("wrong type")));
    assert(bound.PendingDirty() == runtime::Dirty::None);
    assert(bound.SetBinding("surface", contracts::Color{1, 2, 3, 255}));
    assert(bound.PendingDirty() == runtime::Dirty::Paint);
    assert(BuildAndCommit(bound));
    assert(bound.AcceptsBinding("surface", contracts::Color{1, 2, 3, 255}));
    assert(!bound.SetBinding("surface", contracts::Color{1, 2, 3, 255}));
    assert(bound.SetBinding("label", std::string("Hello")));
    assert(runtime::Has(bound.PendingDirty(), runtime::Dirty::Layout));
    assert(BuildAndCommit(bound));
    assert(bound.SetBinding("size", 24.0));
    assert(runtime::Has(bound.PendingDirty(), runtime::Dirty::Layout));
    assert(!bound.SetBinding("size", 0.0));
    assert(!bound.SetProperty(bound.RootId(), runtime::DslProperty::Font, 18.0));
    runtime::Scene action_scene(runtime::ParseBlueprint("Button(\"Go\", action: \"one\")"), shape,
                                contracts::ResourceId{7});
    assert(action_scene.SetViewport({100, 50}));
    assert(BuildAndCommit(action_scene));
    const auto action_snapshot = action_scene.InputGeometry();
    const auto action_pixels = action_scene.PixelsRevision();
    assert(action_scene.SetProperty(action_scene.RootId(), runtime::DslProperty::Action,
                                    std::string("two")));
    assert(action_scene.PendingDirty() == runtime::Dirty::Composite);
    assert(!BuildAndCommit(action_scene));
    assert(action_scene.PixelsRevision() == action_pixels);
    assert(action_scene.InputGeometry()->version > action_snapshot->version);
    assert(action_snapshot->Find(action_scene.RootId())->action == "one");
    assert(action_scene.InputGeometry()->Find(action_scene.RootId())->action == "two");
    assert(action_scene.ActionAt({10, 10}) == "two");
    bool rejected = false;
    try {
        (void)runtime::ParseBlueprint("Slider($value)");
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);

    // Pixel demand survives Build until the presenter has drawn that revision.
    // Protocol-only changes have an independent acknowledgement: they never
    // create a DisplayList or silently clear a pending pixel change.
    runtime::Scene submission(
        runtime::ParseBlueprint(
            "Card(backdropBlur: $blur, inputShape: $shape, background: $tint) { Text($title) }"),
        shape, contracts::ResourceId{7});
    assert(submission.PixelsRevision() == 1);
    assert(submission.SetViewport({160, 90}));
    assert(submission.SetBinding("blur", 12.0));
    assert(submission.SetBinding("shape", std::string("bounds")));
    assert(submission.SetBinding("tint", contracts::Color{10, 20, 30, 120}));
    assert(submission.SetBinding("title", std::string("Initial")));
    const auto initial_pixels = submission.PixelsRevision();
    assert(submission.Build({1}));
    assert(submission.PixelsRevision() == initial_pixels);
    submission.AcknowledgeComposite();
    assert(submission.PendingDirty() == runtime::Dirty::None);
    assert(submission.SetViewport({160, 90}));
    assert(!submission.SetBinding("title", std::string("Initial")));
    assert(submission.PixelsRevision() == initial_pixels && !submission.Build({1}));
    const auto built = submission.GetRenderStats();
    assert(submission.SetBinding("blur", 18.0));
    assert(submission.PendingDirty() == runtime::Dirty::Composite);
    assert(submission.PixelsRevision() == initial_pixels && !submission.Build({1}));
    assert(submission.SurfaceEffects().front().blur_radius == 18);
    assert(submission.PendingDirty() == runtime::Dirty::Composite);
    assert(submission.GetRenderStats().builds == built.builds);
    assert(submission.GetRenderStats().layouts == built.layouts);
    submission.AcknowledgeComposite();
    assert(submission.PendingDirty() == runtime::Dirty::None);
    assert(submission.SetBinding("shape", std::string("visible")));
    assert(submission.PixelsRevision() == initial_pixels);
    assert(submission.SetBinding("title", std::string("Latest")));
    const auto latest_pixels = submission.PixelsRevision();
    assert(latest_pixels > initial_pixels);
    submission.AcknowledgeComposite(); // A state commit must not consume Paint/Layout.
    assert(runtime::Has(submission.PendingDirty(), runtime::Dirty::Layout));
    assert(submission.Build({1}) && submission.PixelsRevision() == latest_pixels);
    submission.AcknowledgeComposite();
    assert(submission.PendingDirty() == runtime::Dirty::None);
}
