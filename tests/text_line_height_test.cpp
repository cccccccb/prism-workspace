#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include <cassert>
#include <stdexcept>

using namespace prism;
using namespace prism::runtime;

namespace {
ShapedText Shape(std::string_view text, double size)
{
    ShapedText result{{}, text.size() * size / 2, size};
    for (std::size_t i = 0; i < text.size(); ++i) {
        result.glyphs.push_back({static_cast<unsigned>(text[i]), {i * size / 2, size * .8}});
    }
    return result;
}

void Reject(std::string_view source)
{
    bool rejected = false;
    try {
        ParseBlueprint(source);
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main()
{
    Scene scene(ParseBlueprint(R"(
        VStack(spacing: 0) {
            Text("a\r\nb\nc", font: 10, lineHeight: $leading)
            Button("Save", action: "save", font: 10, lineHeight: $leading)
        }
    )"),
                Shape);
    scene.SetBinding("leading", 24.0);
    scene.SetViewport({200, 200});
    auto list = scene.Build({1});
    assert(list);
    assert(scene.Bounds({1, 1}).height == 72);
    assert(scene.Bounds({3, 1}).height == 24);
    bool paragraph = false;
    for (const auto &command : list->commands) {
        if (const auto *run = std::get_if<contracts::DrawGlyphRun>(&command);
            run && run->glyphs.size() == 3) {
            assert(run->glyphs[0].origin.y == 15);
            assert(run->glyphs[1].origin.y == 39);
            assert(run->glyphs[2].origin.y == 63);
            paragraph = true;
        }
    }
    assert(paragraph);
    assert(!scene.SetBinding("leading", -1.0));
    assert(!scene.SetBinding("leading", true));
    assert(scene.SetBinding("leading", 2.0));
    assert(scene.Build({1}));
    assert(scene.Bounds({1, 1}).height == 30); // Never overlap font metrics.
    assert(scene.Bounds({3, 1}).height == 10);
    assert(scene.SetBinding("leading", 0.0));
    assert(scene.Build({1}));
    assert(scene.Bounds({1, 1}).height == 30);
    assert(!scene.Build({1}));

    Scene editor(ParseBlueprint(R"(
        TextArea("a\nb", action: "edit", font: 10, lineHeight: 30)
    )"),
                 Shape);
    editor.SetViewport({180, 100});
    assert(editor.Build({1}));
    // A click near the second baseline must edit the second line, not the first.
    editor.HandleInput(contracts::PointerButtonEvent{
        {1}, {0, 44}, contracts::PointerButton::Primary, contracts::ButtonState::Pressed});
    auto edited = editor.HandleInput(contracts::TextInputEvent{{1}, "X"});
    assert(edited.text_edit && edited.text_edit->text == "a\nXb");

    Reject("Text(\"x\", lineHeight: -1)");
    Reject("Text(\"x\", lineHeight: 513)");
    Reject("Text(\"x\", lineHeight: true)");
    Reject("Card(lineHeight: 20) {}");
    Reject("Text(\"x\").transition(property: \"lineHeight\", durationMs: 100)");
}
