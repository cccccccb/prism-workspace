#include "prism/runtime/contour_recipe.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/load_plan.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
using namespace prism::runtime;
using NeckShape = prism::contracts::PanelNeckShape;

const ComponentSource origin{"recipe-fixture", "/package/ui/recipe.prism", "fixture-v1"};
constexpr std::string_view literal_recipe = R"(
Contour(recipe:"attachedPanel",radius:12,neckWidth:24,neckHeight:8,fallback:"detached")
)";
constexpr std::string_view theme_recipe = R"(
Contour(recipe:"attachedPanel",radius:"@panel_radius",neckWidth:"@space_section",
        neckHeight:"@space_sm",fallback:"detached")
)";
constexpr std::string_view rounded_recipe = R"(
Contour(recipe:"attachedPanel",radius:12,neckWidth:40,neckHeight:14,
        neckShape:"roundedTriangle",fallback:"detached")
)";
constexpr std::string_view rectangle = R"(
Contour { Move(x:0,y:0) Line(x:40,y:0) Line(x:40,y:32) Line(x:0,y:32) }
)";

std::string Popup(std::string_view contour)
{
    return "Popup(\"open\",width:160,height:104) { " + std::string(contour) + " Text(\"Body\") }";
}

std::string Recipe(std::string_view fields)
{
    return "Contour(recipe:\"attachedPanel\"," + std::string(fields) + ")";
}

std::string Numbers(std::string_view radius, std::string_view width, std::string_view height)
{
    return Recipe("radius:" + std::string(radius) + ",neckWidth:" + std::string(width) +
                  ",neckHeight:" + std::string(height) + ",fallback:\"detached\"");
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
    const auto prepared = PrepareComponent(Popup(literal_recipe), origin);
    const AttachedPanelRecipe expected{12.0, 24.0, 8.0, ContourFallback::Detached};
    assert(expected.neck_shape == NeckShape::SoftTab);
    assert(prepared.Root().contour_recipe == expected);
    assert(!prepared.Root().contour);
    assert(prepared.Root().children.size() == 1 && prepared.NodeCount() == 2);
    assert(prepared.Root().children.front().kind == Kind::Text);
    assert(prepared.Root().theme_refs.empty() && prepared.Root().bindings.empty());

    const auto linked = LinkComponent(prepared);
    assert(linked.contour_recipe == expected && !linked.contour);
    assert(linked.children.size() == 1 && !linked.children.front().contour_recipe);
    auto independent = LinkComponent(prepared);
    independent.contour_recipe->radius = 2.0;
    assert(prepared.Root().contour_recipe == expected &&
           LinkComponent(prepared).contour_recipe == expected);

    const auto menu = PrepareComponent("Menu(\"menu\") { " + std::string(literal_recipe) +
                                           " MenuItem(action:\"run\") { Text(\"Run\") } }",
                                       origin);
    assert(menu.Root().kind == Kind::Menu && menu.Root().contour_recipe == expected);
    assert(menu.Root().children.size() == 1);

    const auto minimum = ParseBlueprint(Popup(Numbers("0", "0", "0")));
    assert(minimum.contour_recipe ==
           (AttachedPanelRecipe{0.0, 0.0, 0.0, ContourFallback::Detached}));
    const auto maximum = ParseBlueprint(Popup(Numbers("256", "256", "48")));
    assert(maximum.contour_recipe ==
           (AttachedPanelRecipe{256.0, 256.0, 48.0, ContourFallback::Detached}));
    const auto fractional = ParseBlueprint(Popup(Numbers("0.253", "24.25", "7.5")));
    assert(std::get<double>(fractional.contour_recipe->radius) == 0.253);
    assert(std::get<double>(fractional.contour_recipe->neck_width) == 24.25);
    assert(std::get<double>(fractional.contour_recipe->neck_height) == 7.5);

    const auto static_shape = PrepareComponent(Popup(rectangle), origin);
    assert(static_shape.Root().contour && !static_shape.Root().contour_recipe);
    static_assert(static_cast<unsigned>(DslProperty::Last) == 57);
}

void CheckNeckShapes()
{
    const AttachedPanelRecipe expected{12.0, 40.0, 14.0, ContourFallback::Detached,
                                       NeckShape::RoundedTriangle};
    const auto prepared = PrepareComponent(Popup(rounded_recipe), origin);
    assert(prepared.Root().contour_recipe == expected);
    assert(prepared.Root().bindings.empty() && prepared.Root().theme_refs.empty());
    auto linked = LinkComponent(prepared);
    assert(linked.contour_recipe == expected);
    linked.contour_recipe->neck_shape = NeckShape::SoftTab;
    assert(prepared.Root().contour_recipe == expected &&
           LinkComponent(prepared).contour_recipe == expected);

    const auto explicit_default = ParseBlueprint(Popup(
        Recipe("radius:12,neckWidth:24,neckHeight:8,neckShape:\"softTab\",fallback:\"detached\"")));
    assert(explicit_default.contour_recipe == ParseBlueprint(Popup(literal_recipe)).contour_recipe);

    const auto themed = ParseBlueprint(Popup(Recipe(
        "radius:\"@panel_radius\",neckWidth:\"@panel_neck_width\","
        "neckHeight:\"@panel_neck_height\",neckShape:\"roundedTriangle\",fallback:\"detached\"")));
    assert(themed.contour_recipe->neck_shape == NeckShape::RoundedTriangle);
    assert(std::get<ContourThemeNumber>(themed.contour_recipe->neck_width).name ==
           "panel_neck_width");
    assert(std::get<ContourThemeNumber>(themed.contour_recipe->neck_height).name ==
           "panel_neck_height");
}

PreparedComponent OwnThemeRecipe()
{
    auto source = Popup(theme_recipe);
    auto prepared = PrepareComponent(source, origin);
    source.assign(source.size(), 'x');
    return prepared;
}

void CheckReferences()
{
    const AttachedPanelRecipe expected{ContourThemeNumber{"panel_radius"},
                                       ContourThemeNumber{"space_section"},
                                       ContourThemeNumber{"space_sm"}, ContourFallback::Detached};
    const auto prepared = OwnThemeRecipe();
    assert(prepared.Root().contour_recipe == expected);
    assert(prepared.Root().theme_refs.empty() && prepared.Root().bindings.empty());
    const auto linked = LinkComponent(prepared);
    assert(linked.contour_recipe == expected);
    auto independent = LinkComponent(prepared);
    std::get<ContourThemeNumber>(independent.contour_recipe->radius).name = "changed";
    assert(prepared.Root().contour_recipe == expected &&
           LinkComponent(prepared).contour_recipe == expected);

    const auto plain = PrepareComponent(Popup(""), origin);
    std::uint64_t reference_bytes{};
    const auto &recipe = *prepared.Root().contour_recipe;
    reference_bytes += std::get<ContourThemeNumber>(recipe.radius).name.capacity();
    reference_bytes += std::get<ContourThemeNumber>(recipe.neck_width).name.capacity();
    reference_bytes += std::get<ContourThemeNumber>(recipe.neck_height).name.capacity();
    assert(prepared.RetainedBytes() == plain.RetainedBytes() + reference_bytes);
    assert(prepared.NodeCount() == plain.NodeCount());

    const auto mixed = ParseBlueprint(Popup(Numbers("\"@0_panel-radius\"", "24", "8")));
    assert(std::get<ContourThemeNumber>(mixed.contour_recipe->radius).name == "0_panel-radius");
    assert(std::get<double>(mixed.contour_recipe->neck_width) == 24.0);
    const auto maximum_name = std::string(64, 'n');
    const auto named = ParseBlueprint(Popup(Numbers("\"@" + maximum_name + "\"", "24", "8")));
    assert(std::get<ContourThemeNumber>(named.contour_recipe->radius).name == maximum_name);
}

void CheckRejections()
{
    Reject(literal_recipe, "child geometry declaration");
    Reject("Card { " + std::string(literal_recipe) + " }", "only supported on Popup or Menu");
    Reject("InteractionTarget { Visual { " + std::string(literal_recipe) + " } }",
           "only supported on Popup or Menu");
    Reject("Text(\"Label\") { " + std::string(literal_recipe) + " }", "not supported on Text");
    Reject("Popup(\"open\",material:\"window\") { " + std::string(literal_recipe) + " }",
           "compositor window material");
    Reject(Popup(std::string(literal_recipe) + std::string(literal_recipe)), "duplicate Contour");
    Reject(Popup(std::string(literal_recipe) + std::string(rectangle)), "duplicate Contour");
    Reject(Popup(std::string(rectangle) + std::string(literal_recipe)), "duplicate Contour");
    Reject(Popup(std::string(literal_recipe) + " { Move(x:0,y:0) }"), "cannot have children");
    Reject(Popup(std::string(literal_recipe) + ".background(#FFFFFFFF)"),
           "do not accept modifiers");
    Reject(Popup(std::string(literal_recipe) + ".state(when:\"hovered\",radius:8)"),
           "do not accept modifiers");
    Reject(Popup(std::string(literal_recipe) +
                 ".transition(property:\"radius\",durationMs:120,easing:\"linear\")"),
           "do not accept modifiers");

    constexpr std::array missing{"neckWidth:24,neckHeight:8,fallback:\"detached\"",
                                 "radius:12,neckHeight:8,fallback:\"detached\"",
                                 "radius:12,neckWidth:24,fallback:\"detached\"",
                                 "radius:12,neckWidth:24,neckHeight:8"};
    for (const auto fields : missing) {
        Reject(Popup(Recipe(fields)), "missing Contour recipe field");
    }
    Reject(Popup("Contour(radius:12,neckWidth:24,neckHeight:8,fallback:\"detached\")"),
           "unknown or positional Contour field");
    Reject(
        Popup(Recipe("radius:12,neckWidth:24,neckHeight:8,fallback:\"detached\",space:\"local\"")),
        "unknown or positional Contour recipe field: space");
    Reject(Popup(Recipe("radius:12,neckWidth:24,neckHeight:8,fallback:\"detached\",extra:0")),
           "unknown or positional Contour recipe field: extra");
    Reject(Popup(Recipe("radius:12,neckWidth:24,neckHeight:8,fallback:\"detached\",0")),
           "unknown or positional Contour recipe field");
    Reject(Popup(Recipe("radius:12,radius:8,neckWidth:24,neckHeight:8,fallback:\"detached\"")),
           "duplicate Contour recipe field: radius");
    Reject(Popup(Recipe("recipe:\"attachedPanel\",radius:12,neckWidth:24,neckHeight:8,"
                        "fallback:\"detached\"")),
           "duplicate Contour recipe field: recipe");
    Reject(Popup(Recipe("radius:12,neckWidth:24,neckHeight:8,fallback:\"detached\","
                        "fallback:\"detached\"")),
           "duplicate Contour recipe field: fallback");
    Reject(Popup("Contour(recipe:\"other\",radius:12,neckWidth:24,neckHeight:8,"
                 "fallback:\"detached\")"),
           "recipe requires the literal attachedPanel");
    Reject(Popup("Contour(recipe:attachedPanel,radius:12,neckWidth:24,neckHeight:8,"
                 "fallback:\"detached\")"),
           "recipe requires the literal attachedPanel");
    Reject(Popup(Recipe("radius:12,neckWidth:24,neckHeight:8,fallback:\"attached\"")),
           "fallback requires the literal detached");
    Reject(Popup(Recipe("radius:12,neckWidth:24,neckHeight:8,fallback:$mode")),
           "fallback requires the literal detached");

    constexpr std::array invalid_shapes{
        "\"\"",   "\"other\"",       "\"SoftTab\"",           "1",     "true",
        "$shape", "roundedTriangle", "\"@panel_neck_shape\"", "[1,2]", "#FFFFFFFF"};
    for (const auto value : invalid_shapes) {
        Reject(Popup(Recipe("radius:12,neckWidth:24,neckHeight:8,fallback:\"detached\","
                            "neckShape:" +
                            std::string(value))),
               "neckShape requires the literal softTab or roundedTriangle");
    }
    Reject(Popup(Recipe("radius:12,neckWidth:24,neckHeight:8,fallback:\"detached\","
                        "neckShape:\"softTab\",neckShape:\"roundedTriangle\"")),
           "duplicate Contour recipe field: neckShape");
    Reject("Popup(\"open\") {\n Contour(recipe:\"attachedPanel\",radius:12,\n"
           " neckShape:$shape,neckWidth:24,neckHeight:8,fallback:\"detached\")\n}",
           "neckShape requires the literal", 3);

    constexpr std::array invalid_numbers{"-0.001",  "256.001", "\"12\"", "true",
                                         "$radius", "radius",  "[1,2]",  "#FFFFFFFF"};
    for (const auto value : invalid_numbers) {
        Reject(Popup(Numbers(value, "24", "8")), "finite numeric literal");
    }
    Reject(Popup(Numbers("12", "256.001", "8")), "within 0..256");
    Reject(Popup(Numbers("12", "24", "48.001")), "within 0..48");
    Reject(Popup(Numbers("12", "-0.001", "8")), "within 0..256");
    Reject(Popup(Numbers("12", "24", "-0.001")), "within 0..48");
    Reject(Popup(Numbers(std::string(400, '9'), "24", "8")), "finite numeric literal");
    constexpr std::array invalid_references{"\"@\"", "\"@panel.radius\"", "\"@panel radius\"",
                                            "\"@panel/radius\"", "\"@圆角\""};
    for (const auto value : invalid_references) {
        Reject(Popup(Numbers(value, "24", "8")), "valid @theme number name");
    }
    Reject(Popup(Numbers("\"@" + std::string(65, 'n') + "\"", "24", "8")), "at most 64 bytes");
    Reject("Popup(\"open\") {\n Contour(recipe:\"attachedPanel\",\n radius:$radius,\n"
           " neckWidth:24,neckHeight:8,fallback:\"detached\")\n}",
           "finite numeric literal", 3);
}

void CheckComposition(bool geometry_before)
{
    const auto plan = CompileLoadPlan(R"(
        Interface(version:2,layout:"ui/layout.prism") {
            Component(id:"body",source:"ui/body.prism",phase:"critical")
        }
    )",
                                      {"master", "/package/master.prism", "v1"}, "/package");
    const auto body = PrepareComponent("Menu(\"inner\") { " + std::string(rounded_recipe) +
                                           " MenuItem(action:\"run\") { Text(\"Run\") } }",
                                       {"body", "/package/ui/body.prism", "v1"});
    const std::string slot = "Slot(component:\"body\",width:160,height:104) { Text(\"Pending\") }";
    const auto source =
        "Popup(\"outer\",width:240,height:180) { " +
        (geometry_before ? std::string(theme_recipe) + slot : slot + std::string(theme_recipe)) +
        " }";
    const auto layout = PrepareLayout(source, plan, {"layout", "/package/ui/layout.prism", "v1"});
    assert(layout.Slots().size() == 1);
    assert(layout.Slots().front().node_path == (std::vector<std::size_t>{0}));
    assert(layout.Body().Root().children.size() == 1 && layout.Body().NodeCount() == 3);
    assert(layout.Body().Root().contour_recipe && !layout.Body().Root().contour);
    const std::array units{PreparedUnit{"body", body}};
    const auto composed = ComposeCritical(plan, layout, units);
    const auto blueprint = LinkComponent(composed);
    assert(blueprint.contour_recipe == layout.Body().Root().contour_recipe);
    assert(!blueprint.contour && blueprint.children.size() == 1);
    const auto &wrapper = blueprint.children.front();
    assert(wrapper.region == "body" && wrapper.region_mounted);
    assert(!wrapper.contour_recipe && wrapper.children.size() == 1);
    assert(wrapper.children.front().contour_recipe == body.Root().contour_recipe);
    assert(!wrapper.children.front().contour);

    auto independent = LinkComponent(composed);
    std::get<ContourThemeNumber>(independent.contour_recipe->radius).name = "changed";
    independent.children.front().children.front().contour_recipe->neck_height = 2.0;
    independent.children.front().children.front().contour_recipe->neck_shape = NeckShape::SoftTab;
    assert(LinkComponent(composed).contour_recipe == layout.Body().Root().contour_recipe);
    assert(LinkComponent(composed).children.front().children.front().contour_recipe ==
           body.Root().contour_recipe);
}
} // namespace

int main()
{
    CheckPreparation();
    CheckNeckShapes();
    CheckReferences();
    CheckRejections();
    CheckComposition(true);
    CheckComposition(false);
    std::puts("DSL attached panel recipe validation/ownership/Slot composition cases passed");
}
