#include "prism/runtime/dsl_frontend.hpp"
#include <cassert>
#include <stdexcept>
#include <variant>

using namespace prism;

int main() {
    int shape_calls = 0;
    auto shape = [&shape_calls](std::string_view text, double size) {
        ++shape_calls;
        runtime::ShapedText result;
        result.width = text.size() * size * 0.5;
        result.height = size * 1.4;
        for (std::size_t i = 0; i < text.size(); ++i)
            result.glyphs.push_back({static_cast<std::uint32_t>(static_cast<unsigned char>(text[i])),
                {i * size * 0.5, size}});
        return result;
    };
    auto blueprint = runtime::ParseBlueprint(
        "HStack(spacing: 8) { Button(\"Launch\", action: \"app:launch\", width: 100) "
        "VStack { Text($title, font: 20) Card(width: 40, height: 30).cornerRadius(6) } }");
    blueprint.properties.push_back({runtime::DslProperty::Background, contracts::Color{12, 24, 36, 255}});
    blueprint.properties.push_back({runtime::DslProperty::Clip, true});
    runtime::Scene scene(std::move(blueprint), shape, contracts::ResourceId{7});
    assert(scene.SetViewport({300, 120}));
    auto first = scene.Build(contracts::WindowId{1});
    assert(first && first->generation == 1);
    assert(!scene.Build(contracts::WindowId{1})); // idle pages do not submit
    assert(scene.ActionAt({20, 20}) == "app:launch");
    assert(!scene.ActionAt({180, 20}));
    assert(std::holds_alternative<contracts::PushClipRect>(first->commands.front()));
    assert(std::holds_alternative<contracts::PopClip>(first->commands.back()));
    assert(scene.SetSlot("title", "Prism"));
    assert(runtime::Has(scene.PendingDirty(), runtime::Dirty::Layout));
    auto second = scene.Build(contracts::WindowId{1});
    assert(second && second->generation == 2);
    bool found = false;
    for (const auto& command : second->commands) {
        if (auto* run = std::get_if<contracts::DrawGlyphRun>(&command)) {
            if (run->glyphs.size() == 5 && run->font.value == 7) found = true;
        }
    }
    assert(found);
    assert(!scene.SetSlot("title", "Prism"));
    assert(!scene.Build(contracts::WindowId{1}));
    assert(scene.SetBackground(scene.RootId(), {20, 30, 40, 255}));
    assert(scene.PendingDirty() == runtime::Dirty::Paint);
    const int before_paint = shape_calls;
    assert(scene.Build(contracts::WindowId{1}));
    assert(shape_calls == before_paint);
    assert(!scene.SetViewport({0, 120}));
    assert(!scene.Build(contracts::WindowId{1}));
    assert(scene.Bounds({999, 1}).width == 0);
    runtime::Scene bound(runtime::ParseBlueprint(
        "Card(background: $surface) { Text($label, font: $size) }"), shape, contracts::ResourceId{7});
    assert(bound.SetViewport({120, 80}));
    assert(bound.Build(contracts::WindowId{1}));
    assert(!bound.SetBackground(bound.RootId(), contracts::Color{0, 0, 0, 0}));
    assert(!bound.AcceptsBinding("missing", std::string("value")));
    assert(!bound.AcceptsBinding("surface", std::string("wrong type")));
    assert(!bound.SetBinding("surface", std::string("wrong type")));
    assert(bound.PendingDirty() == runtime::Dirty::None);
    assert(bound.SetBinding("surface", contracts::Color{1, 2, 3, 255}));
    assert(bound.PendingDirty() == runtime::Dirty::Paint);
    assert(bound.Build(contracts::WindowId{1}));
    assert(bound.AcceptsBinding("surface", contracts::Color{1, 2, 3, 255}));
    assert(!bound.SetBinding("surface", contracts::Color{1, 2, 3, 255}));
    assert(bound.SetBinding("label", std::string("Hello")));
    assert(runtime::Has(bound.PendingDirty(), runtime::Dirty::Layout));
    assert(bound.Build(contracts::WindowId{1}));
    assert(bound.SetBinding("size", 24.0));
    assert(runtime::Has(bound.PendingDirty(), runtime::Dirty::Layout));
    assert(!bound.SetBinding("size", 0.0));
    assert(!bound.SetProperty(bound.RootId(), runtime::DslProperty::Font, 18.0));
    runtime::Scene action_scene(runtime::ParseBlueprint(
        "Button(\"Go\", action: \"one\")"), shape, contracts::ResourceId{7});
    assert(action_scene.SetViewport({100, 50}));
    assert(action_scene.Build(contracts::WindowId{1}));
    assert(action_scene.SetProperty(action_scene.RootId(), runtime::DslProperty::Action,
        std::string("two")));
    assert(action_scene.PendingDirty() == runtime::Dirty::None);
    assert(!action_scene.Build(contracts::WindowId{1}));
    assert(action_scene.ActionAt({10, 10}) == "two");
    bool rejected = false;
    try { (void)runtime::ParseBlueprint("Slider($value)"); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}
