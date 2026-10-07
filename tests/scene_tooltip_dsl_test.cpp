#include "../prism/runtime/scene_p.hpp"
#include "prism/compiler/lexer.hpp"
#include "prism/compiler/parser.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/layout_engine.hpp"
#include "prism/runtime/load_plan.hpp"
#include "prism/runtime/owner_feedback_panel.hpp"
#include "prism/runtime/owner_file_panel.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "prism/runtime/scene_snapshot.hpp"
#include "prism/theme/compiler.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace prism;
using namespace prism::runtime;

namespace {
ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font / 2, font};
}

std::string Source(std::string_view contents = "Text(\"Save document\", font: 14)",
                   std::string_view properties = "width: 160, height: 44, padding: 8")
{
    return "VStack(spacing: 0) { Button(\"Save\", action: \"save\", height: 32) "
           "Button(\"Other\", action: \"other\", height: 32) Tooltip(\"save\", " +
           std::string(properties) + ") { " + std::string(contents) + " } }";
}

void Rejected(std::string_view source)
{
    bool rejected = false;
    try {
        PrepareComponent(source);
    } catch (const LoadFailure &error) {
        assert(error.Diagnostic().stage == LoadStage::Semantic);
        rejected = true;
    }
    assert(rejected);
}

void RejectedBlueprint(Blueprint blueprint)
{
    bool rejected = false;
    try {
        Scene scene(std::move(blueprint), Shape);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

void Property(Blueprint &node, DslProperty id, PropertyValue value)
{
    for (auto &property : node.properties) {
        if (property.id == id) {
            property.value = std::move(value);
            return;
        }
    }
    node.properties.push_back({id, std::move(value)});
}

void SchemaAndGrammar()
{
    static_assert(IsFloatingKind(Kind::Tooltip));
    static_assert(!IsPopupKind(Kind::Tooltip));
    static_assert(static_cast<unsigned>(DslProperty::Last) < 64);
    const auto prepared = PrepareComponent(Source());
    assert(prepared.Root().children.back().kind == Kind::Tooltip);
    assert(FindComponent("Tooltip")->positional == DslProperty::TooltipFor);
    assert(FindProperty("tooltipDelayMs")->id == DslProperty::TooltipDelayMs);
    assert(ValidPropertyValue(DslProperty::TooltipDelayMs, 0.0));
    assert(ValidPropertyValue(DslProperty::TooltipDelayMs, 10000.0));
    assert(!ValidPropertyValue(DslProperty::TooltipDelayMs, -1.0));
    assert(!ValidPropertyValue(DslProperty::TooltipDelayMs, 10001.0));
    assert(!ValidPropertyValue(DslProperty::TooltipDelayMs, true));

    PrepareComponent(Source("VStack(spacing: 4) { Text(\"Save document\", font: 14) "
                            "HStack(spacing: 4) { Icon(\"save\", width: 16, height: 16) "
                            "Text(\"Ctrl+S\", font: 11) } }",
                            "width: 180, height: 64, padding: 8, tooltipDelayMs: 0"));
    PrepareComponent(
        Source("Text($tip, font: 14)", "width: 160, height: 44, tooltipDelayMs: $delay"));
    PrepareComponent(Source("Text(\"保存文件\\nCtrl+S\", font: 14)"));
    PrepareComponent(R"(
Card {
 Button("Save", action: "save", height: 32)
 Button("Menu", action: "menu", height: 32)
 Tooltip("save", width: 160, height: 44) { Text("Save") }
 Popup("menu", width: 180, height: 100) { Button("Done", action: "done") }
}
)");

    Rejected("Tooltip(\"save\", width: 160, height: 44) { Text(\"Save\") }");
    Rejected(Source("Text(\"Save\")", "width: 160, height: 44, tooltipFor: $anchor"));
    Rejected(Source("Text(\"Save\")", "width: 160, height: 44, tooltipDelayMs: -1"));
    Rejected(Source("Text(\"Save\")", "width: 160, height: 44, tooltipDelayMs: 10001"));
    Rejected(Source("Text(\"Save\")", "width: 0, height: 44"));
    Rejected(Source("Text(\"Save\")", "height: 44"));
    Rejected(Source("Text(\"Save\")", "width: 160"));
    Rejected("Card(tooltipFor: \"save\") { Text(\"Save\") }");
    Rejected("VStack { Button(\"Save\", action: \"save\") Card { "
             "Tooltip(\"save\", width: 160, height: 44) { Text(\"Save\") } } }");
    Rejected("VStack { Button(\"Save\", action: \"save\") "
             "Tooltip(\"save\", width: 160, height: 44) { Text(\"Save\") } Text(\"After\") }");
    Rejected("VStack { Button(\"Save\", action: \"save\") Button(\"Copy\", action: \"save\") "
             "Tooltip(\"save\", width: 160, height: 44) { Text(\"Save\") } }");
    Rejected("VStack { Button(\"Save\", action: $save) "
             "Tooltip(\"save\", width: 160, height: 44) { Text(\"Save\") } }");
    Rejected("VStack { Visual { Card { Button(\"Save\", action: \"save\") } } "
             "Tooltip(\"save\", width: 160, height: 44) { Text(\"Save\") } }");
    Rejected("VStack { Button(\"Menu\", action: \"menu\") "
             "Popup(\"menu\", width: 180, height: 100) { Button(\"Save\", action: \"save\") } "
             "Tooltip(\"save\", width: 160, height: 44) { Text(\"Save\") } }");

    for (const auto content :
         {"Button(\"Save\", action: \"inside\")", "IconButton(\"save\", action: \"inside\")",
          "Toggle(action: \"inside\")", "Checkbox(action: \"inside\")",
          "TextField(\"text\", action: \"inside\")", "TextArea(\"text\", action: \"inside\")",
          "InteractionTarget { Text(\"Inside\") }", "ScrollView { VStack { Text(\"Inside\") } }",
          "Card(action: \"\") { Text(\"Inside\") }",
          "Tooltip(\"save\", width: 120, height: 40) { Text(\"Nested\") }",
          "Card(material: \"window\") { Text(\"Inside\") }"}) {
        Rejected(Source(content));
    }
    Rejected(Source("Text(\"" + std::string(257, 'a') + "\")"));
    Rejected(Source("VStack { Text(\"" + std::string(200, 'a') + "\") Text(\"" +
                    std::string(57, 'b') + "\") }"));
    Rejected(Source("Text(\"Bad\ttext\")"));
    Rejected(Source("Text(\"" + std::string("\xc0\xaf", 2) + "\")"));
}

void DirectBlueprintAndProjection()
{
    auto blueprint = ParseBlueprint(Source());
    Property(blueprint.children.back(), DslProperty::TooltipFor, std::string("missing"));
    RejectedBlueprint(std::move(blueprint));

    blueprint = ParseBlueprint(Source());
    Property(blueprint.children.back(), DslProperty::TooltipFor, std::string("save"));
    blueprint.children.back().bindings.push_back({"anchor", DslProperty::TooltipFor});
    RejectedBlueprint(std::move(blueprint));

    blueprint = ParseBlueprint(Source());
    blueprint.children.back().region = "foreign";
    RejectedBlueprint(std::move(blueprint));

    blueprint = ParseBlueprint(Source());
    Property(blueprint.children.back().children.front(), DslProperty::Action, std::string{});
    RejectedBlueprint(std::move(blueprint));

    blueprint = ParseBlueprint(Source());
    Property(blueprint.children.back().children.front(), DslProperty::Text, std::string(257, 'x'));
    RejectedBlueprint(std::move(blueprint));

    blueprint = ParseBlueprint(Source());
    Property(blueprint.children.front(), DslProperty::TooltipDelayMs, 500.0);
    RejectedBlueprint(std::move(blueprint));

    Scene scene(ParseBlueprint(Source("Text($tip, font: 14)",
                                      "width: 160, height: 44, tooltipDelayMs: $delay")),
                Shape);
    scene.SetViewport({320, 240});
    assert(scene.Preflight({{"tip", std::string("Save document")}, {"delay", 650.0}}));
    const auto tooltip = scene.root_->children.back()->id;
    assert(scene.Find(tooltip)->tooltip_delay_ms == 650);
    const auto before = scene.TransactionRevision();
    assert(!scene.Preflight({{"tip", std::string(257, 'x')}}));
    assert(scene.TransactionRevision() == before);
    assert(scene.Find(tooltip)->children.front()->text == "Save document");
    assert(scene.Preflight({{"delay", 0.0}}));
    assert(scene.Find(tooltip)->tooltip_delay_ms == 0);
    assert(!scene.Preflight({{"delay", 10001.0}}));
}

// This exercises the real Scene snapshot and LayoutEngine only. Setting the private
// active IDs models an already-selected tooltip, not native input or UI adoption.
SceneSnapshot Geometry(std::string_view source, contracts::LogicalSize viewport)
{
    Scene scene(ParseBlueprint(source), Shape);
    scene.SetViewport(viewport);
    scene.ResolveLayout();
    for (const auto *node : scene.nodes_) {
        if (!node) {
            continue;
        }
        if (node->kind == Kind::Tooltip) {
            scene.active_tooltip_ = node->id;
        }
        if (node->action == "save") {
            scene.tooltip_anchor_ = node->id;
        }
    }
    auto snapshot = scene.CaptureResolvedSnapshot();
    LayoutEngine::Compute(snapshot, viewport, Shape);
    return snapshot;
}

const SnapshotNode &Tooltip(const SceneSnapshot &snapshot)
{
    for (const auto &node : snapshot.nodes) {
        if (node.kind == Kind::Tooltip) {
            return node;
        }
    }
    throw std::runtime_error("Missing test Tooltip");
}

void GeometryAndReadability()
{
    const auto below = Geometry(Source(), {320, 240});
    const auto &tooltip = Tooltip(below);
    assert(tooltip.style.visible && tooltip.bounds.width == 160 && tooltip.bounds.height == 44);
    assert(tooltip.bounds.y == 40);
    const auto other = below.Get(below.root).children[1];
    assert(below.Get(other).bounds.y == 32); // Tooltip never enters normal flow.
    assert(tooltip.bounds.x >= 8 && tooltip.bounds.x + tooltip.bounds.width <= 312);

    const auto above = Geometry(R"(
VStack(spacing: 0) {
 Card(height: 140)
 Button("Save", action: "save", height: 32)
 Tooltip("save", width: 160, height: 44, padding: 8) { Text("Save", font: 14) }
}
)",
                                {320, 220});
    assert(Tooltip(above).style.visible && Tooltip(above).bounds.y < 140);

    const auto right = Geometry(R"(
Card {
 Button("Save", action: "save", anchor: "right", width: 32, height: 32)
 Tooltip("save", width: 160, height: 44, padding: 8) { Text("Save", font: 14) }
}
)",
                                {320, 240});
    assert(Tooltip(right).style.visible && Tooltip(right).bounds.x == 152);

    assert(!Tooltip(Geometry(Source(), {120, 240})).style.visible);
    assert(!Tooltip(Geometry(Source(), {320, 60})).style.visible);
    assert(!Tooltip(Geometry(Source("Text(\"Save\", font: 14)",
                                    "width: 160, height: 44, padding: 0, cornerRadius: 22"),
                             {320, 240}))
                .style.visible);
    assert(!Tooltip(Geometry(Source("Text(\"This label cannot fit in this card\", font: 14)"),
                             {320, 240}))
                .style.visible);
    assert(!Tooltip(Geometry(Source("Visual(translateX: 200, height: 14) { "
                                    "Text(\"Save\", font: 14) }"),
                             {320, 240}))
                .style.visible);
    assert(Tooltip(Geometry(Source("Visual(scaleX: 0.8, scaleY: 0.8, height: 14) { "
                                   "Text(\"Save\", font: 14) }"),
                            {320, 240}))
               .style.visible);
    assert(
        Tooltip(Geometry(Source("Text(\"Save\\nCtrl+S\", font: 14)"), {320, 240})).style.visible);
    assert(!Tooltip(Geometry(Source("Text(\"Save\\nCtrl+S\", font: 14)",
                                    "width: 160, height: 40, padding: 8"),
                             {320, 240}))
                .style.visible);
    const auto clipped = Geometry(R"(
VStack(spacing: 0) {
 ScrollView(height: 32) { VStack(spacing: 0) {
  Card(height: 40)
  Button("Save", action: "save", height: 32)
 } }
 Tooltip("save", width: 160, height: 44, padding: 8) { Text("Save", font: 14) }
}
)",
                                  {320, 240});
    assert(!Tooltip(clipped).style.visible);
    const auto rounded = Geometry(R"(
Card(clip: true, cornerRadius: 90) {
 Button("Save", action: "save", anchor: "right", width: 32, height: 32)
 Tooltip("save", width: 160, height: 44, padding: 8) { Text("Save", font: 14) }
}
)",
                                  {240, 180});
    assert(!Tooltip(rounded).style.visible);
}

std::string Read(const std::filesystem::path &path)
{
    std::ifstream input(path);
    assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

void RejectedLayout(std::string_view source, const LoadPlan &plan)
{
    bool rejected = false;
    try {
        PrepareLayout(source, plan);
    } catch (const LoadFailure &error) {
        assert(error.Diagnostic().stage == LoadStage::Semantic);
        rejected = true;
    }
    assert(rejected);
}

void RejectedComposition(const LoadPlan &plan, const PreparedLayout &layout,
                         const std::vector<PreparedUnit> &units)
{
    bool rejected = false;
    try {
        ComposeCritical(plan, layout, units);
    } catch (const LoadFailure &error) {
        assert(error.Diagnostic().stage == LoadStage::Semantic);
        rejected = true;
    }
    assert(rejected);
}

void PendingRegionAnchorComposition()
{
    const auto plan = CompileLoadPlan(R"(
Interface(version:2, layout:"layout.prism") {
    Component(id:"toolbar", source:"toolbar.prism", phase:"critical")
}
)",
                                      {"master", "/package/master.prism", {}}, "/package");
    constexpr auto layout_source = R"(
Card {
    Slot(component:"toolbar")
    Tooltip("save", width:160, height:44, padding:8) { Text("Save", font:14) }
}
)";
    Rejected(layout_source); // An ordinary component still cannot declare a Slot.
    const auto layout = PrepareLayout(layout_source, plan);
    const auto &placeholder = layout.Body().Root().children.front();
    assert(placeholder.region == "toolbar" && !placeholder.region_mounted);
    RejectedBlueprint(
        LinkComponent(layout.Body())); // A final Scene never permits a missing anchor.

    const std::vector<PreparedUnit> valid_units{
        {"toolbar", PrepareComponent("Button(\"Save\", action:\"save\", height:32)")}};
    const auto composed = ComposeCritical(plan, layout, valid_units);
    assert(composed.Root().children.front().region_mounted);
    Scene scene(LinkComponent(composed), Shape);
    assert(scene.RegionMounted("toolbar"));

    const std::vector<PreparedUnit> missing_units{{"toolbar", PrepareComponent("Text(\"Empty\")")}};
    RejectedComposition(plan, layout, missing_units);
    Rejected("Card { Button(\"Other\", action:\"other\") "
             "Tooltip(\"missing\", width:160, height:44) { Text(\"Missing\") } }");

    RejectedLayout(R"(
Card {
    Slot(component:"toolbar")
    Button("Save", action:"save")
    Button("Duplicate", action:"save")
    Tooltip("save", width:160, height:44) { Text("Save") }
}
)",
                   plan);
    RejectedLayout(R"(
Card {
    Slot(component:"toolbar")
    Visual { Button("Save", action:"save") }
    Tooltip("save", width:160, height:44) { Text("Save") }
}
)",
                   plan);

    const auto duplicate_layout = PrepareLayout(R"(
Card {
    Slot(component:"toolbar")
    Button("Save", action:"save")
    Tooltip("save", width:160, height:44) { Text("Save") }
}
)",
                                                plan);
    RejectedComposition(plan, duplicate_layout, valid_units); // Expansion reveals the ambiguity.

    const auto deferred_plan = CompileLoadPlan(R"(
Interface(version:2, layout:"layout.prism") {
    Component(id:"toolbar", source:"toolbar.prism", phase:"critical")
    Component(id:"later", source:"later.prism", phase:"deferred")
}
)",
                                               {"master", "/package/master.prism", {}}, "/package");
    const auto deferred_layout = PrepareLayout(R"(
Card {
    Slot(component:"toolbar")
    Slot(component:"later")
    Tooltip("later-command", width:160, height:44) { Text("Later") }
}
)",
                                               deferred_plan);
    RejectedComposition(deferred_plan, deferred_layout, valid_units);
}

void ActualNotepadCriticalComposition(const std::filesystem::path &root)
{
    const auto package = root / "prism-notepad";
    const auto master = package / "master.prism";
    const auto plan = CompileLoadPlan(Read(master), {"master", master.string(), {}}, package);
    assert(!plan.legacy);
    const auto path = package / plan.layout_path;
    const auto layout = PrepareLayout(Read(path), plan, {"layout", path.string(), {}});
    assert(layout.Slots().size() == 3);

    std::vector<PreparedUnit> units;
    for (const auto &unit : plan.components) {
        if (unit.phase == LoadPhase::Critical) {
            const auto source = package / unit.source_path;
            units.push_back(
                {unit.id, PrepareComponent(Read(source), {unit.id, source.string(), {}})});
        }
    }
    const auto composed = ComposeCritical(plan, layout, units);
    Scene scene(LinkComponent(composed), Shape, {},
                theme::LoadTheme(root / "resources/themes", "square", 1, "light"));
    assert(scene.RegionMounted("toolbar") && scene.RegionMounted("editor") &&
           scene.RegionMounted("status"));
    std::size_t tooltips{};
    for (const auto &node : composed.Root().children) {
        tooltips += node.kind == Kind::Tooltip;
    }
    assert(tooltips == 2); // Real application source, including its critical toolbar actions.
}

void CheckOrder(const Blueprint &root)
{
    bool floating = false;
    bool shared = false;
    for (const auto &child : root.children) {
        if (IsFloatingKind(child.kind)) {
            floating = true;
        } else {
            assert(!floating);
            shared = shared || !child.region.empty();
        }
    }
    assert(shared && floating);
}

void SharedComposition(const std::filesystem::path &root)
{
    const auto app = ParseBlueprint(R"(
Card {
 Button("Save", action: "save", height: 32)
 Button("Menu", action: "menu", height: 32)
 Tooltip("save", width: 160, height: 44) { Text("Save") }
 Popup("menu", width: 180, height: 100) { Button("Done", action: "done") }
}
)");
    CheckOrder(ComposeOwnerTaskPanel(
        app, ParseBlueprint(Read(root / "resources/ui/owner-task-panel.prism"))));
    const auto file = ParseBlueprint(Read(root / "resources/ui/owner-file-panel.prism"));
    CheckOrder(ComposeOwnerTaskPanels(app, nullptr, &file));
    CheckOrder(ComposeOwnerFeedbackPanel(
        app, ParseBlueprint(Read(root / "resources/ui/owner-feedback-panel.prism"))));
}

void LegacyRejects()
{
    compiler::Lexer lexer(Source());
    const auto tokens = lexer.Tokenize();
    compiler::Parser parser(tokens);
    bool rejected = false;
    try {
        parser.Parse();
    } catch (const std::runtime_error &error) {
        assert(std::string_view(error.what()).find("legacy .prismb") != std::string_view::npos);
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    SchemaAndGrammar();
    DirectBlueprintAndProjection();
    GeometryAndReadability();
    PendingRegionAnchorComposition();
    ActualNotepadCriticalComposition(argv[1]);
    SharedComposition(argv[1]);
    LegacyRejects();
}
