#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/theme/compiler.hpp"

#include <cassert>
#include <cmath>
#include <limits>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr auto source = R"(
ScrollView(scrollSpeed: 1, background: #172231FF) {
    VStack(spacing: 0) {
        Button("Alpha", action: "a", height: 40)
        Button("Beta", action: "b", height: 40)
        Button("Gamma", action: "c", height: 40)
        Button("Delta", action: "d", height: 40)
        Button("Epsilon", action: "e", height: 40, visible: $tail)
        Button("Zeta", action: "f", height: 40, visible: $tail)
    }
    Visual(scrollPart: "thumb", width: 4, height: 20, inset: 3, background: #83B9FFFF)
}
)";

ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font / 2, font};
}

struct Fixture {
    Scene scene{ParseBlueprint(source), Shape};
    std::shared_ptr<const InputSnapshot> shown;

    Fixture()
    {
        scene.SetBinding("tail", true);
        scene.SetViewport({220, 80});
        Present();
    }

    void Present()
    {
        scene.Build({1});
        shown = scene.InputGeometry();
        scene.ApplyInputSnapshot(shown);
    }

    double Offset() const
    {
        return scene.ScrollInfo(scene.RootId())->offset;
    }

    InteractionResult Wheel(double amount)
    {
        return scene.HandleInput(
            contracts::PointerScrollEvent{{1}, {10, 10}, 0, amount, 1, {1, 1, 1}}, shown);
    }

    InteractionResult Button(bool down)
    {
        return scene.HandleInput(
            contracts::PointerButtonEvent{{1},
                                          {10, 10},
                                          contracts::PointerButton::Primary,
                                          down ? contracts::ButtonState::Pressed
                                               : contracts::ButtonState::Released,
                                          0,
                                          1,
                                          {1, 1, 1}},
            shown);
    }

    void Tab(bool reverse = false)
    {
        contracts::KeyEvent key{{1},       0x2b, contracts::ButtonState::Pressed, false, 1,
                                {1, 2, 1}, {}};
        key.modifiers.shift = reverse;
        scene.HandleInput(key, shown);
    }
};

void GeometryAndInput()
{
    Fixture f;
    auto info = f.scene.ScrollInfo(f.scene.RootId());
    assert(info && info->content_height == 240 && info->viewport_height == 80 &&
           info->maximum == 160);
    assert(f.scene.ActionAt({10, 10}) == "a");
    assert(!f.scene.HitTest({10, 90}));
    const auto layouts = f.scene.GetRenderStats().layouts;
    f.Button(true);
    assert(f.Wheel(40).changed);
    assert(f.Offset() == 40);
    assert(!Has(f.scene.PendingDirty(), Dirty::Layout));
    assert(!f.Button(false).activation);
    f.Button(true); // Old displayed offset cannot arm a new activation in moved content.
    assert(!f.Button(false).activation);
    assert(f.Wheel(40).changed); // Wheel bursts can continue while the frame is pending.
    assert(f.Offset() == 80);
    f.Present();
    assert(f.scene.GetRenderStats().layouts == layouts);
    assert(f.scene.ActionAt({10, 10}) == "c");
    f.Button(true);
    const auto release = f.Button(false);
    assert(release.activation && release.activation->action == "c");
    f.Wheel(1e300);
    assert(f.Offset() == 160);
    f.Present();
    const auto pixels = f.scene.PixelsRevision();
    assert(!f.Wheel(10).changed);
    assert(f.scene.PixelsRevision() == pixels);
    f.Wheel(-1e300);
    assert(f.Offset() == 0);
    assert(!f.scene.ScrollTo(f.scene.RootId(), std::numeric_limits<double>::quiet_NaN()));
    assert(!f.scene.SetProperty(f.scene.RootId(), DslProperty::Clip, false));
}

void FocusAndTransactions()
{
    Fixture f;
    f.Tab();
    f.Tab();
    f.Tab(); // Gamma was outside the viewport.
    assert(f.Offset() == 40);
    f.Present();
    f.Tab();
    assert(f.Offset() == 80);
    f.Present();
    f.Tab(true);
    assert(f.Offset() == 80);
    f.Tab(true);
    assert(f.Offset() == 40);
    f.Present();
    assert(f.scene.Preflight({{"tail", true}}));
    assert(f.Offset() == 40);
    f.Present();
    f.scene.ScrollTo(f.scene.RootId(), 160);
    assert(f.scene.Preflight({{"tail", false}}));
    assert(f.Offset() == 80 && f.scene.ScrollInfo(f.scene.RootId())->content_height == 160);
    f.scene.SetViewport({220, 400});
    f.Present();
    assert(f.Offset() == 0 && f.scene.ScrollInfo(f.scene.RootId())->maximum == 0);
    assert(!f.Wheel(20).changed);
}

void Nested()
{
    Scene scene(ParseBlueprint(R"(
ScrollView(scrollSpeed: 1) {
 VStack(spacing: 0) {
  ScrollView(height: 60, scrollSpeed: 1) {
   VStack(spacing: 0) {
    Button("First", action: "first", height: 40)
    Button("Second", action: "second", height: 40)
    Button("Third", action: "third", height: 40)
   }
  }
  Button("After", action: "after", height: 100)
 }
}
)"),
                Shape);
    scene.SetViewport({200, 100});
    scene.Build({1});
    const auto snapshot = scene.InputGeometry();
    contracts::NodeId inner;
    for (const auto &node : snapshot->nodes) {
        if (node.id != scene.RootId() && scene.ScrollInfo(node.id)) {
            inner = node.id;
        }
    }
    assert(inner);
    scene.HandleInput(contracts::PointerScrollEvent{{1}, {10, 10}, 0, 90, 1, {1, 1, 1}}, snapshot);
    assert(scene.ScrollInfo(inner)->offset == 60);
    assert(scene.ScrollInfo(scene.RootId())->offset == 30);
    // Negative residual goes back through the same inner-to-outer order.
    scene.HandleInput(contracts::PointerScrollEvent{{1}, {10, 10}, 0, -90, 1, {1, 1, 1}}, snapshot);
    assert(scene.ScrollInfo(inner)->offset == 0 && scene.ScrollInfo(scene.RootId())->offset == 0);
}

void SliderCancellation()
{
    Scene scene(ParseBlueprint(R"(
ScrollView(scrollSpeed: 1) {
 VStack(spacing: 0) {
  Slider(action: "volume", value: 0.2, height: 44) {
   Visual(sliderPart: "track", height: 4)
   Visual(sliderPart: "fill", height: 4)
   Visual(sliderPart: "thumb", width: 16, height: 16)
  }
  Button("After", action: "after", height: 100)
 }
}
)"),
                Shape);
    scene.SetViewport({200, 80});
    scene.Build({1});
    const auto snapshot = scene.InputGeometry();
    scene.HandleInput(contracts::PointerButtonEvent{{1},
                                                    {100, 22},
                                                    contracts::PointerButton::Primary,
                                                    contracts::ButtonState::Pressed,
                                                    0,
                                                    1,
                                                    {1, 1, 1}},
                      snapshot);
    assert(scene.TakeControlEvents().front().event.phase == ValuePhase::Preview);
    scene.HandleInput(contracts::PointerScrollEvent{{1}, {100, 22}, 0, 20, 1, {1, 1, 1}}, snapshot);
    auto events = scene.TakeControlEvents();
    assert(events.size() == 1 && events.front().event.phase == ValuePhase::Cancel);
    assert(events.front().event.reason == ValueCancelReason::Unavailable);
    scene.HandleInput(contracts::PointerButtonEvent{{1},
                                                    {100, 22},
                                                    contracts::PointerButton::Primary,
                                                    contracts::ButtonState::Released,
                                                    0,
                                                    1,
                                                    {1, 1, 1}},
                      snapshot);
    assert(scene.TakeControlEvents().empty());
}

void ThemeAndValidation(const char *root)
{
    Scene scene(ParseBlueprint(source), Shape, {}, theme::LoadTheme(root, "glass", 1, "dark"));
    scene.SetBinding("tail", true);
    scene.SetViewport({220, 80});
    scene.Build({1});
    scene.ScrollTo(scene.RootId(), 100);
    assert(scene.ApplyTheme(theme::LoadTheme(root, "square", 2, "light")));
    assert(scene.ScrollInfo(scene.RootId())->offset == 100);

    for (const auto bad :
         {"ScrollView", "ScrollView { VStack VStack }", "ScrollView(padding: 8) { VStack }",
          "ScrollView(scrollSpeed: 0) { VStack }",
          "Card { Visual(scrollPart: \"thumb\", width: 4) }", "ScrollView { VStack Visual }",
          "ScrollView { VStack Visual(scrollPart: $role, width: 4) }"}) {
        bool rejected = false;
        try {
            Scene invalid(ParseBlueprint(bad), Shape);
        } catch (const std::exception &) {
            rejected = true;
        }
        assert(rejected);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    GeometryAndInput();
    FocusAndTransactions();
    Nested();
    SliderCancellation();
    ThemeAndValidation(argv[1]);
}
