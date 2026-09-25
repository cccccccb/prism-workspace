#include "prism/runtime/dsl_frontend.hpp"
#include <cassert>
#include <stdexcept>
#include <variant>

using namespace prism;

int main() {
    auto shape = [](std::string_view text, double size) {
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
    blueprint.style.background = {12, 24, 36, 255};
    blueprint.style.clip = true;
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
    assert(scene.Build(contracts::WindowId{1}));
    assert(!scene.SetViewport({0, 120}));
    assert(!scene.Build(contracts::WindowId{1}));
    assert(scene.Bounds({999, 1}).width == 0);
    bool rejected = false;
    try { (void)runtime::ParseBlueprint("Slider($value)"); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}
