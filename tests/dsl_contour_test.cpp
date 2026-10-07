#include "prism/contracts/contour.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/load_plan.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace prism::runtime;
using prism::contracts::Contour;

const ComponentSource origin{"contour-fixture", "/package/ui/contour.prism", "fixture-v1"};
constexpr std::string_view rectangle = R"(
Contour {
    Move(x:0,y:0)
    Line(x:40,y:0)
    Line(x:40,y:32)
    Line(x:0,y:32)
}
)";

Contour Rectangle()
{
    return {{{0, 0}, {40, 0}, {40, 32}, {0, 32}}};
}

std::string Card(std::string_view contour)
{
    return "Card(width:80,height:64) { " + std::string(contour) + " Text(\"Body\") }";
}

void Reject(std::string_view source, std::string_view expected, int line = 0)
{
    bool rejected = false;
    try {
        PrepareComponent(source, origin);
    } catch (const LoadFailure &error) {
        rejected = true;
        const auto &diagnostic = error.Diagnostic();
        assert(diagnostic.stage == LoadStage::Semantic);
        assert(diagnostic.source.component_id == origin.component_id);
        assert(diagnostic.source.source_path == origin.source_path);
        assert(diagnostic.source.source_version == origin.source_version);
        assert(diagnostic.line > 0);
        if (line) {
            assert(diagnostic.line == line);
        }
        if (diagnostic.message.find(expected) == std::string::npos) {
            std::fprintf(stderr, "expected: %.*s\nactual: %s\n", int(expected.size()),
                         expected.data(), diagnostic.message.c_str());
        }
        assert(diagnostic.message.find(expected) != std::string::npos);
    }
    assert(rejected);
}

void CheckPreparation()
{
    const auto prepared = PrepareComponent(Card(rectangle), origin);
    assert(prepared.Root().contour == Rectangle());
    assert(prepared.Root().children.size() == 1 && prepared.NodeCount() == 2);
    assert(prepared.Root().children.front().kind == Kind::Text);
    assert(prepared.Root().properties.size() == 3); // width, height and Card spacing.
    const auto linked = LinkComponent(prepared);
    assert(linked.contour == prepared.Root().contour && linked.children.size() == 1);

    auto independent = LinkComponent(prepared);
    independent.contour->points[0].x = 1;
    assert(prepared.Root().contour == Rectangle() &&
           LinkComponent(prepared).contour == Rectangle());
    const auto plain = PrepareComponent("Card(width:80,height:64) { Text(\"Body\") }", origin);
    assert(prepared.RetainedBytes() >=
           plain.RetainedBytes() +
               prepared.Root().contour->points.capacity() * sizeof(prism::contracts::LogicalPoint));

    const auto literal = ParseBlueprint(R"(
        Card(width:80,height:64,cornerRadius:8) {
            Contour(space:"local") {
                Move(x:0.001,y:0)
                Line(x:40.001,y:0)
                Line(x:40.001,y:32)
                Line(x:0.001,y:32)
            }
        }
    )");
    assert(literal.contour == Rectangle() && literal.children.empty());

    // Geometry is independent of scalar theme/binding properties. Radius can
    // remain in the style contract; the explicit contour owns the node shape.
    const auto theme = PrepareComponent(
        "Card(cornerRadius:\"@surface.radius\") { " + std::string(rectangle) + " }", origin);
    assert(theme.Root().contour == Rectangle());
    assert(theme.Root().theme_refs ==
           (std::vector<ThemeRef>{{"surface.radius", DslProperty::Radius}}));
    const auto binding =
        PrepareComponent("Card(cornerRadius:$radius) { " + std::string(rectangle) + " }", origin);
    assert(binding.Root().contour == Rectangle() && binding.Root().bindings.size() == 1);

    const auto button = ParseBlueprint("Button(\"Run\",\"run\",width:80,height:64) { " +
                                       std::string(rectangle) + " }");
    assert(button.contour == Rectangle() && button.children.size() == 1);
    assert(button.children.front().kind == Kind::Text && !button.children.front().contour);

    const auto visual =
        PrepareComponent("InteractionTarget { Visual { " + std::string(rectangle) +
                             " }.state(when:\"hovered\",scope:\"target\",opacity:0.8) "
                             ".transition(property:\"opacity\",durationMs:120,easing:\"linear\") }",
                         origin);
    assert(visual.Root().children.front().contour == Rectangle());
    assert(visual.Root().children.front().state_rules.size() == 1);
    assert(visual.Root().children.front().transitions.size() == 1);

    constexpr std::array supported{
        Kind::Box,   Kind::Row,  Kind::Column,   Kind::Visual,  Kind::InteractionTarget,
        Kind::Popup, Kind::Menu, Kind::MenuItem, Kind::MenuBack};
    for (const auto kind : supported) {
        assert(SupportsContour(kind));
    }
    constexpr std::array unsupported{
        Kind::Text,     Kind::Image,    Kind::Icon,       Kind::IconButton,
        Kind::Progress, Kind::Toggle,   Kind::Separator,  Kind::TextField,
        Kind::TextArea, Kind::Checkbox, Kind::RadioGroup, Kind::SegmentGroup,
        Kind::Radio,    Kind::Segment,  Kind::Slider,     Kind::ScrollView};
    for (const auto kind : unsupported) {
        assert(!SupportsContour(kind));
    }
    assert(!SupportsContour(static_cast<Kind>(100)));
    static_assert(static_cast<unsigned>(DslProperty::Last) == 57);
}

void CheckCurves()
{
    const auto prepared = PrepareComponent(R"(
        Card(width:40,height:32) {
            Contour {
                Move(x:8,y:0)
                Line(x:32,y:0)
                Cubic(c1x:36.418,c1y:0,c2x:40,c2y:3.582,x:40,y:8)
                Line(x:40,y:24)
                Cubic(c1x:40,c1y:28.418,c2x:36.418,c2y:32,x:32,y:32)
                Line(x:8,y:32)
                Cubic(c1x:3.582,c1y:32,c2x:0,c2y:28.418,x:0,y:24)
                Line(x:0,y:8)
                Cubic(c1x:0,c1y:3.582,c2x:3.582,c2y:0,x:8,y:0)
            }
        }
    )",
                                           origin);
    assert(prepared.Root().contour && prepared.Root().contour->points.size() > 8);
    prism::contracts::ValidateContour(*prepared.Root().contour);
    const auto bounds = prism::contracts::ContourBounds(*prepared.Root().contour);
    assert(bounds.x == 0 && bounds.y == 0 && bounds.width == 40 && bounds.height == 32);
    for (const auto point : prepared.Root().contour->points) {
        assert(std::floor(point.x * 256) == point.x * 256);
        assert(std::floor(point.y * 256) == point.y * 256);
    }
    assert(LinkComponent(prepared).contour == prepared.Root().contour);
}

void CheckRejections()
{
    Reject(rectangle, "child geometry declaration");
    Reject("Card { Move(x:0,y:0) }", "only allowed inside Contour");
    Reject("Card { Line(x:0,y:0) }", "only allowed inside Contour");
    Reject("Card { Cubic(x:0,y:0) }", "only allowed inside Contour");
    Reject("Text(\"Label\") { " + std::string(rectangle) + " }", "not supported on Text");
    Reject("Slider { " + std::string(rectangle) + " }", "not supported on Slider");
    Reject("ScrollView { " + std::string(rectangle) + " }", "not supported on ScrollView");
    Reject("Card(material:\"window\") { " + std::string(rectangle) + " }",
           "compositor window material");
    Reject("Slider { Visual(sliderPart:\"thumb\") { " + std::string(rectangle) + " } }",
           "generated Slider/ScrollView Visual parts");
    Reject("ScrollView { Visual(scrollPart:\"thumb\") { " + std::string(rectangle) + " } }",
           "generated Slider/ScrollView Visual parts");
    Reject(Card(std::string(rectangle) + std::string(rectangle)), "duplicate Contour");
    Reject("Card.contour($path)", "unsupported client DSL modifier: contour");
    Reject("InteractionTarget { Visual.state(when:\"hovered\",scope:\"target\",contour:0) }",
           "state property not supported");
    Reject("InteractionTarget { "
           "Visual.transition(property:\"contour\",durationMs:100,easing:\"linear\") }",
           "property cannot transition");
    Reject(Card("Contour(space:\"normalized\") { Move(x:0,y:0) Line(x:1,y:0) Line(x:0,y:1) }"),
           "normalized is unsupported");
    Reject(Card("Contour(space:$space) { Move(x:0,y:0) Line(x:1,y:0) Line(x:0,y:1) }"),
           "literal local");
    Reject(Card("Contour(space:\"@shape\") { Move(x:0,y:0) Line(x:1,y:0) Line(x:0,y:1) }"),
           "literal local");
    Reject(Card("Contour(foo:1) {}"), "unknown or positional Contour field");
    Reject(Card("Contour(\"local\") {}"), "unknown or positional Contour field");
    Reject(Card("Contour(space:\"local\",space:\"local\") {}"), "duplicate Contour field");
    Reject(Card("Contour.background(#FFFFFFFF)"), "do not accept modifiers");
    Reject(Card("Contour {}"), "initial Move and path segments");
    Reject(Card("Contour { Line(x:0,y:0) }"), "must start with Move");
    Reject(Card("Contour { Move(x:0,y:0) Move(x:1,y:1) }"), "exactly one initial Move");
    Reject(Card("Contour { Move(x:0,y:0) }"), "Invalid Contour geometry");
    Reject(Card("Contour { Move(x:0,y:0) Close() }"), "unsupported Contour command");
    Reject(Card("Contour { Move(x:0,y:0) Line(x:1,y:0) { Text(\"Bad\") } }"),
           "cannot have children");
    Reject(Card("Contour { Move(x:0,y:0) Line(x:1,y:0).translateX(2) }"),
           "do not accept modifiers");
    Reject(Card("Contour { Move(0,0) }"), "unknown or positional Move field");
    Reject(Card("Contour { Move(x:0,x:1,y:0) }"), "duplicate Move field");
    Reject(Card("Contour { Move(x:0) }"), "missing Move coordinate: y");
    Reject(Card("Contour { Move(x:0,y:0) Cubic(c1x:0,c1y:0,c2x:1,c2y:1,x:2) }"),
           "missing Cubic coordinate: y");
    Reject(Card("Contour { Move(x:0,y:0) Cubic(cx:0,x:1,y:1) }"),
           "unknown or positional Cubic field");

    constexpr std::array wrong_values{"$point", "\"@point\"", "\"3\"",     "true",
                                      "point",  "[1,2]",      "#FFFFFFFF", "8193"};
    for (const auto value : wrong_values) {
        Reject(
            Card("Contour { Move(x:" + std::string(value) + ",y:0) Line(x:4,y:0) Line(x:0,y:4) }"),
            "finite numeric literal");
    }
    Reject(
        Card("Contour { Move(x:" + std::string(400, '9') + ",y:0) Line(x:4,y:0) Line(x:0,y:4) }"),
        "finite numeric literal");
    Reject("Card {\n Contour {\n Move(x:$point,y:0)\n Line(x:4,y:0)\n Line(x:0,y:4)\n }\n}",
           "finite numeric literal", 3);
    Reject(Card("Contour { Move(x:0,y:0) Line(x:4,y:4) Line(x:0,y:4) Line(x:4,y:0) }"),
           "Invalid Contour geometry");
    Reject(Card("Contour { Move(x:0,y:0) Line(x:0.001,y:0) Line(x:0,y:0.001) }"),
           "Invalid Contour geometry");

    std::string excessive = "Contour { Move(x:0,y:0) ";
    for (unsigned index = 0; index < 257; ++index) {
        excessive += "Line(x:1,y:1) ";
    }
    excessive += "}";
    Reject(Card(excessive), "segment count exceeds 256");
}

void CheckComposition(bool geometry_before)
{
    const auto plan = CompileLoadPlan(R"(
        Interface(version:2,layout:"ui/layout.prism") {
            Component(id:"first",source:"ui/first.prism",phase:"critical")
            Component(id:"second",source:"ui/second.prism",phase:"critical")
        }
    )",
                                      {"master", "/package/master.prism", "v1"}, "/package");
    const auto first =
        PrepareComponent(Card(rectangle), {"first", "/package/ui/first.prism", "v1"});
    const auto second =
        PrepareComponent(Card(rectangle), {"second", "/package/ui/second.prism", "v1"});
    const std::string slots =
        "Slot(component:\"first\",width:80,height:64) { " + std::string(rectangle) +
        " Text(\"Pending first\") } VStack { " + std::string(rectangle) +
        " Slot(component:\"second\",width:80,height:64) { Text(\"Pending second\") } } ";
    const auto source =
        "HStack(width:240,height:128) { " +
        (geometry_before ? std::string(rectangle) + slots : slots + std::string(rectangle)) + " }";
    const auto layout = PrepareLayout(source, plan, {"layout", "/package/ui/layout.prism", "v1"});
    assert(layout.Slots().size() == 2);
    assert(layout.Slots()[0].node_path == (std::vector<std::size_t>{0}));
    assert(layout.Slots()[1].node_path == (std::vector<std::size_t>{1, 0}));
    assert(layout.Body().Root().children.size() == 2);
    assert(layout.Body().Root().contour == Rectangle());
    const std::array units{PreparedUnit{"first", first}, PreparedUnit{"second", second}};
    const auto composed = ComposeCritical(plan, layout, units);
    const auto blueprint = LinkComponent(composed);
    assert(blueprint.contour == Rectangle());
    const auto &first_slot = blueprint.children[0];
    assert(first_slot.region == "first" && first_slot.region_mounted);
    assert(first_slot.contour == Rectangle());
    assert(first_slot.children.size() == 1 && first_slot.children.front().contour == Rectangle());
    const auto &nested = blueprint.children[1];
    assert(nested.contour == Rectangle());
    assert(nested.children.front().region == "second" && nested.children.front().region_mounted);
    assert(nested.children.front().children.front().contour == Rectangle());
}
} // namespace

int main()
{
    CheckPreparation();
    CheckCurves();
    CheckRejections();
    CheckComposition(true);
    CheckComposition(false);
    std::puts("DSL contour literals/ownership/diagnostics/Slot composition cases passed");
}
