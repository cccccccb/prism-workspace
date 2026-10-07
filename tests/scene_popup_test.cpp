#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/theme/compiler.hpp"
#include <cassert>

using namespace prism;
using namespace prism::runtime;

namespace {
ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font / 2, font};
}

constexpr auto source = R"(
VStack(spacing: 0, background: #172231FF) {
 Button("Sound", action: "sound", height: 40, visible: $available)
 Button("Background", action: "background", height: 40)
 Popup("sound", width: 240, height: 180, background: #25364AFF, cornerRadius: 8) {
  VStack(padding: 12, spacing: 8) {
   Button("Apply", action: "apply", height: 40)
   Checkbox(action: "mute", checked: false, height: 40)
   Slider(action: "volume", value: 0.5, height: 44) {
    Visual(sliderPart: "track", height: 4)
    Visual(sliderPart: "fill", height: 4)
    Visual(sliderPart: "thumb", width: 16, height: 16)
   }
  }
 }
}
)";

struct Fixture {
    Scene scene{ParseBlueprint(source), Shape};
    std::shared_ptr<const InputSnapshot> shown;
    contracts::NodeId anchor;

    Fixture()
    {
        scene.SetBinding("available", true);
        scene.SetViewport({320, 300});
        Present();
        anchor = Find("sound");
    }

    void Present()
    {
        scene.Build({1});
        shown = scene.InputGeometry();
        scene.ApplyInputSnapshot(shown);
    }

    contracts::NodeId Find(std::string_view action)
    {
        for (const auto &node : shown->nodes) {
            if (node.action == action) {
                return node.id;
            }
        }
        return {};
    }

    InteractionResult Button(contracts::LogicalPoint point, bool down)
    {
        return scene.HandleInput(
            contracts::PointerButtonEvent{{1},
                                          point,
                                          contracts::PointerButton::Primary,
                                          down ? contracts::ButtonState::Pressed
                                               : contracts::ButtonState::Released,
                                          0,
                                          1,
                                          {1, 1, 1}},
            shown);
    }

    InteractionResult Click(contracts::LogicalPoint point)
    {
        Button(point, true);
        return Button(point, false);
    }

    void Open()
    {
        assert(!Click({10, 20}).activation);
        assert(scene.PopupToken());
        Present();
    }

    void Escape(bool down, bool repeat = false)
    {
        scene.HandleInput(contracts::KeyEvent{{1},
                                              0x29,
                                              down ? contracts::ButtonState::Pressed
                                                   : contracts::ButtonState::Released,
                                              repeat,
                                              1,
                                              {1, 2, 1},
                                              {}},
                          shown);
    }
};

void Behavior()
{
    Fixture f;
    const auto background = f.Find("background");
    const auto bounds = f.scene.Bounds(background);
    assert(!f.scene.IsVisible(f.Find("apply")));
    f.Open();
    assert(f.scene.Bounds(background) == bounds); // Floating content consumes no flow space.
    const auto token = f.scene.PopupToken();
    const auto old = f.shown;
    f.Button({300, 60}, true); // Outside press over the background button.
    assert(!f.scene.PopupToken());
    f.Present();
    assert(!f.Button({300, 60}, false).activation);
    assert(f.scene.State(f.anchor).focused);
    assert(f.Click({300, 60}).activation->action == "background");
    f.Open();
    assert(f.scene.PopupToken() != token);
    const auto current = f.shown;
    f.shown = old;
    assert(!f.Click({40, 80}).activation); // Stale popup coordinates cannot execute a command.
    f.shown = current;
    const auto apply = f.scene.Bounds(f.Find("apply"));
    const auto action = f.Click({apply.x + 10, apply.y + 10});
    assert(action.activation && action.activation->action == "apply");
    assert(!f.scene.PopupToken());
    f.Present();
    f.Open();
    const auto mute = f.scene.Bounds(f.Find("mute"));
    assert(f.Click({mute.x + 10, mute.y + 10}).control_edit);
    assert(f.scene.PopupToken()); // Values retain the popup.
    const auto volume = f.scene.Bounds(f.Find("volume"));
    f.Button({volume.x + 50, volume.y + 20}, true);
    assert(!f.scene.TakeControlEvents().empty());
    f.Button({300, 250}, false); // Drag release outside is not an outside press.
    const auto edits = f.scene.TakeControlEvents();
    assert(edits.size() == 1 && edits.front().event.phase == ValuePhase::Commit);
    assert(f.scene.PopupToken());
    f.Escape(true);
    assert(!f.scene.PopupToken());
    f.Present();
    f.Escape(true, true);
    f.Escape(false);
    assert(f.scene.State(f.anchor).focused);
}

void ClippedAnchor()
{
    Scene scene(ParseBlueprint(R"(
VStack(spacing: 0) {
 ScrollView(height: 80, scrollSpeed: 1) {
  VStack(spacing: 0) {
   Button("Open", action: "open", height: 40)
   Button("Other", action: "other", height: 160)
  }
 }
 Popup("open", width: 200, height: 100) { Button("Done", action: "done") }
}
)"),
                Shape);
    scene.SetViewport({320, 300});
    scene.Build({1});
    contracts::NodeId anchor, scroll;
    for (const auto &node : scene.InputGeometry()->nodes) {
        if (node.action == "open") {
            anchor = node.id;
        }
        if (scene.ScrollInfo(node.id)) {
            scroll = node.id;
        }
    }
    assert(anchor && scroll && scene.OpenPopup(anchor));
    scene.Build({1});
    assert(scene.ScrollTo(scroll, 80));
    assert(!scene.PopupToken());
    scene.Build({1});
    assert(!scene.OpenPopup(anchor));
    assert(scene.ScrollTo(scroll, 0));
    scene.Build({1});
    assert(scene.OpenPopup(anchor));
    scene.SetViewport({16, 16});
    scene.Build({1});
    assert(!scene.PopupToken());
}

void Lifetime(const char *themes)
{
    Fixture f;
    f.Open();
    const auto token = f.scene.PopupToken();
    assert(f.scene.Preflight({{"available", true}}));
    f.Present();
    assert(f.scene.PopupToken() == token);
    assert(f.scene.ApplyTheme(theme::LoadTheme(themes, "square", 1, "light")));
    f.Present();
    assert(f.scene.PopupToken() == token);
    f.scene.SetBinding("available", false);
    f.Present();
    assert(!f.scene.PopupToken());
    assert(!f.scene.State(f.anchor).focused);

    for (auto bad : {"Popup(\"x\", width: 200, height: 100)",
                     "Card { Popup(\"x\", width: 200, height: 100) Button(\"x\", action: \"x\") }",
                     "Card { Button(\"x\", action: \"x\") Popup(\"x\") }",
                     "Card { Popup(\"missing\", width: 200, height: 100) }"}) {
        bool failed = false;
        try {
            Scene scene(ParseBlueprint(bad), Shape);
        } catch (const std::exception &) {
            failed = true;
        }
        assert(failed);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    Behavior();
    Lifetime(argv[1]);
    ClippedAnchor();
}
