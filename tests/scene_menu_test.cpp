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
VStack {
 Button("Files", action: "files", height: 36)
 Menu("files", width: 240, height: 220) {
  VStack(spacing: 4) {
   MenuItem(action: "open", height: 36) { Text("Open") }
   MenuItem(action: "paste", height: 36, enabled: false) { Text("Paste") }
   Checkbox(action: "hidden", height: 36, checked: false)
   Slider(action: "scale", value: 0.5, height: 44) {
    Visual(sliderPart: "track", height: 4)
    Visual(sliderPart: "fill", height: 4)
    Visual(sliderPart: "thumb", width: 16, height: 16)
   }
   MenuItem(action: "sort", height: 36, enabled: $sorting) { Text("Sort") }
  }
 }
 Menu("sort", width: 240, height: 220) {
  VStack(spacing: 4) {
   MenuBack(height: 36) { Text("Back") }
   MenuItem(action: "name", height: 36) { Text("Name") }
   MenuItem(action: "date", height: 36) { Text("Date") }
  }
 }
}
)";

struct Fixture {
    Scene scene{ParseBlueprint(source), Shape};
    std::shared_ptr<const InputSnapshot> shown;

    Fixture()
    {
        scene.SetBinding("sorting", true);
        scene.SetViewport({320, 300});
        Present();
    }

    void Present()
    {
        scene.Build({1});
        shown = scene.InputGeometry();
        scene.ApplyInputSnapshot(shown);
    }

    contracts::NodeId Find(std::string_view action) const
    {
        for (const auto &item : shown->nodes) {
            if (item.action == action) {
                return item.id;
            }
        }
        return {};
    }

    InteractionResult Key(std::uint32_t key, bool down = true, bool repeat = false)
    {
        return scene.HandleInput(contracts::KeyEvent{{1},
                                                     key,
                                                     down ? contracts::ButtonState::Pressed
                                                          : contracts::ButtonState::Released,
                                                     repeat,
                                                     1,
                                                     {7, 2, 1},
                                                     {}},
                                 shown);
    }

    InteractionResult Click(contracts::NodeId node)
    {
        const auto b = scene.Bounds(node);
        contracts::PointerButtonEvent event{{1},
                                            {b.x + 8, b.y + 8},
                                            contracts::PointerButton::Primary,
                                            contracts::ButtonState::Pressed,
                                            0,
                                            1,
                                            {7, 1, 1}};
        scene.HandleInput(event, shown);
        event.state = contracts::ButtonState::Released;
        return scene.HandleInput(event, shown);
    }

    void Open()
    {
        assert(!Click(Find("files")).activation);
        assert(scene.PopupToken());
        Present();
        assert(scene.State(Find("open")).focused);
    }
};

void Navigation()
{
    Fixture f;
    f.Open();
    const auto root_token = f.scene.PopupToken();
    const auto old = f.shown;
    f.Key(0x51);
    assert(f.scene.State(f.Find("sort")).focused); // Skip disabled and value rows.
    f.Key(0x4f);
    assert(f.scene.PopupToken() != root_token);
    f.Present();
    assert(f.scene.State(f.Find("$prism.menu.back")).focused);
    assert(!f.scene.IsVisible(f.Find("open")));
    const auto child_token = f.scene.PopupToken();
    f.Key(0x50);
    f.Present();
    assert(f.scene.State(f.Find("sort")).focused);
    assert(f.scene.PopupToken() != root_token && f.scene.PopupToken() != child_token);
    auto current = f.shown;
    f.shown = old;
    assert(!f.Click(f.Find("open")).activation); // Previous parent presentation is stale too.
    f.shown = current;
    f.Key(0x4f);
    f.Present();
    assert(!f.Click(f.Find("$prism.menu.back")).activation);
    f.Present();
    f.Key(0x4f);
    f.Present();
    f.Key(0x51);
    assert(f.scene.State(f.Find("name")).focused);
    f.Key(0x28);
    auto command = f.Key(0x28, false);
    assert(command.activation && command.activation->action == "name");
    assert(!f.scene.PopupToken() && f.scene.State(f.Find("files")).focused);
}

void ValuesAndLifetime(const char *themes)
{
    Fixture f;
    f.Open();
    assert(f.Click(f.Find("hidden")).control_edit);
    assert(f.scene.PopupToken());
    f.Key(0x2b);
    assert(f.scene.State(f.Find("scale")).focused);
    f.Key(0x50);
    f.Key(0x50, false);
    assert(!f.scene.TakeControlEvents().empty() && f.scene.PopupToken());
    f.Key(0x2b); // Tab reaches the next row from the focused value control.
    assert(f.scene.State(f.Find("sort")).focused);
    f.Key(0x4f);
    f.Present();
    auto token = f.scene.PopupToken();
    assert(f.scene.ApplyTheme(theme::LoadTheme(themes, "glass", 1, "light")));
    f.Present();
    assert(f.scene.PopupToken() == token);
    assert(f.scene.Preflight({{"sorting", true}}));
    f.Present();
    assert(f.scene.PopupToken() == token);
    f.Key(0x29);
    f.Present();
    assert(f.scene.PopupToken() && f.scene.State(f.Find("sort")).focused);
    f.Key(0x29, true, true);
    assert(f.scene.PopupToken());
    f.Key(0x29, false);
    f.Key(0x4f);
    f.Present();
    f.scene.SetBinding("sorting", false);
    f.Present();
    assert(!f.scene.PopupToken()); // A suspended parent trigger still controls lifetime.
}

void Invalid()
{
    for (auto text :
         {"MenuItem(action: \"x\") { Text(\"x\") }", "MenuBack { Text(\"Back\") }",
          "Card { Button(\"x\", action: \"x\") Menu(\"x\", width: 200, height: 200) { MenuItem } }",
          "Card { Button(\"x\", action: \"x\") Menu(\"x\", width: 200, height: 200) { "
          "MenuItem(action: \"sub\") } Menu(\"sub\", width: 200, height: 200) { Text(\"Missing "
          "Back\") } }",
          "Card { Menu(\"a\", width: 200, height: 200) { MenuBack MenuItem(action: \"b\") } "
          "Menu(\"b\", width: 200, height: 200) { MenuBack MenuItem(action: \"a\") } }"}) {
        bool failed = false;
        try {
            Scene scene(ParseBlueprint(text), Shape);
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
    Navigation();
    ValuesAndLifetime(argv[1]);
    Invalid();
}
