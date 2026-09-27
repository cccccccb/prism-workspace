#include "prism/runtime/load_plan.hpp"
#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace prism::runtime;

constexpr std::string_view interface_source = R"(
Interface(version:2,layout:"ui/layout.prism") {
    Binding(name:"label",type:"string",initial:"Initial label")
    Binding(name:"progress",type:"number",initial:0.25)
    Binding(name:"ready",type:"bool",initial:false)
    Binding(name:"ink",type:"color",initial:#112233FF)
    Component(id:"first",source:"ui/first.prism",phase:"critical")
    Component(id:"second",source:"ui/second.prism",phase:"critical",after:["first"])
    Component(id:"later",source:"ui/later.prism",phase:"deferred",after:["second"])
}
)";

constexpr std::string_view layout_source = R"(
HStack(spacing:12) {
    Slot(component:"first",width:80) { Image("unused-first.png") }
    Slot(component:"second",flex:1) { Image("unused-second.png") }
    Slot(component:"later",width:120) {
        VStack { Text("Deferred pending") Image("checker.png") Text($label) }
    }
}
)";

LoadPlan Plan(std::string_view text = interface_source)
{
    return CompileLoadPlan(text, {"master", "/package/master.prism", "version-a"}, "/package");
}

PreparedComponent Unit(std::string_view source, std::string id)
{
    return PrepareComponent(source, {id, "/package/ui/" + id + ".prism", "version-a"});
}

void Fails(const std::function<void()> &action, LoadStage stage = LoadStage::Semantic)
{
    bool failed = false;
    try {
        action();
    } catch (const LoadFailure &error) {
        failed = true;
        const auto &diagnostic = error.Diagnostic();
        assert(diagnostic.stage == stage);
        assert(!diagnostic.message.empty());
        assert(!diagnostic.source.source_path.empty());
    }
    assert(failed);
}

struct CompileAttempt {
    std::string text;

    void operator()() const
    {
        Plan(text);
    }
};

struct LayoutAttempt {
    const LoadPlan &plan;
    std::string text;

    void operator()() const
    {
        PrepareLayout(text, plan);
    }
};

struct ValidateAttempt {
    const LoadPlan &plan;
    std::string id;
    PreparedComponent prepared;

    void operator()() const
    {
        ValidatePreparedUnit(plan, id, prepared);
    }
};

struct ComposeAttempt {
    const LoadPlan &plan;
    PreparedLayout layout;
    std::vector<PreparedUnit> units;

    void operator()() const
    {
        ComposeCritical(plan, layout, units);
    }
};

struct OrdinaryAttempt {
    std::string text;

    void operator()() const
    {
        PrepareComponent(text, {"ordinary", "/package/ordinary.prism", {}});
    }
};

struct HeaderAttempt {
    std::string text;

    void operator()() const
    {
        IsInterfaceSource(text, {"master", "/package/master.prism", "version-a"});
    }
};

struct LegacyAttempt {
    PreparedComponent prepared;
    ComponentSource metadata;
    std::filesystem::path root;

    void operator()() const
    {
        CompileLegacyLoadPlan(prepared, metadata, root);
    }
};

std::string One(std::string_view declaration)
{
    return "Interface(version:2,layout:\"layout.prism\") { " + std::string(declaration) + " }";
}

void CheckEntryRecognition()
{
    const ComponentSource metadata{"master", "/package/master.prism", "requested-version"};
    assert(IsInterfaceSource("\n// Prefix comment\n\tInterface(version:2)", metadata));
    assert(IsInterfaceSource("Interface", metadata));
    assert(!IsInterfaceSource("// Interface is only a comment\nText(\"broken\"", metadata));
    assert(!IsInterfaceSource("InterfaceOther(\"still visual\")", metadata));
    assert(!IsInterfaceSource("\nVStack {", metadata));
    Fails(HeaderAttempt{"// Only comment\n"}, LoadStage::Syntax);
    Fails(HeaderAttempt{"\n42"}, LoadStage::Syntax);
    Fails(HeaderAttempt{"{ Text(\"no identifier\") }"}, LoadStage::Syntax);
    Fails(HeaderAttempt{std::string(kMaxLoadFileBytes + 1, ' ')});

    const auto cached = PrepareComponent("Text($untyped_legacy)");
    const auto plan = CompileLegacyLoadPlan(cached, metadata, "/package");
    assert(plan.legacy && plan.version == 1 && plan.components.size() == 1);
    assert(plan.source.component_id == "master" && plan.source.source_path == metadata.source_path);
    assert(plan.source.source_version == "requested-version");
    assert(plan.source_bytes == cached.SourceBytes());
    assert(plan.components[0].id == "master" && plan.components[0].source_path == "master.prism");
    assert(plan.bindings.empty() && cached.Root().bindings[0].name == "untyped_legacy");

    const auto other_origin = Unit("Text(\"cached\")", "different-origin");
    const auto relabeled = CompileLegacyLoadPlan(other_origin, metadata, "/package");
    assert(relabeled.source.source_path == metadata.source_path);
    assert(other_origin.Source().component_id == "different-origin");
    Fails(LegacyAttempt{cached, metadata, {}});
    Fails(LegacyAttempt{cached, {"master", "/outside/master.prism", {}}, "/package"});
    Fails(LegacyAttempt{cached, {"master", "../master.prism", {}}, "/package"});
    auto moved = PrepareComponent("Text(\"moved\")");
    const auto keeper = std::move(moved);
    assert(keeper);
    Fails(LegacyAttempt{std::move(moved), metadata, "/package"});
    Fails(CompileAttempt{"UnknownVisual(\"must be rejected by the full API\")"});
}

void CheckPlan()
{
    const auto plan = Plan();
    assert(plan.version == 2 && !plan.legacy && plan.components.size() == 3);
    assert(plan.source_bytes == interface_source.size());
    assert(plan.package_root == "/package" && plan.layout_path == "ui/layout.prism");
    assert(plan.bindings.size() == 4);
    assert(std::get<std::string>(plan.bindings[0].initial) == "Initial label");
    assert(std::get<double>(plan.bindings[1].initial) == .25);
    assert(!std::get<bool>(plan.bindings[2].initial));
    assert((std::get<prism::contracts::Color>(plan.bindings[3].initial) ==
            prism::contracts::Color{0x11, 0x22, 0x33, 0xFF}));
    assert(FindLoadUnit(plan, "later")->phase == LoadPhase::Deferred);
    assert(FindLoadUnit(plan, "later")->after == std::vector<std::string>{"second"});
    assert(!FindLoadUnit(plan, "missing"));

    const std::vector<std::string> invalid{
        "Interface(version:1,layout:\"x\") { Component(id:\"a\",source:\"a\",phase:\"critical\") }",
        "Interface(version:true,layout:\"x\") { "
        "Component(id:\"a\",source:\"a\",phase:\"critical\") }",
        "Interface(version:2,version:2,layout:\"x\") { "
        "Component(id:\"a\",source:\"a\",phase:\"critical\") }",
        "Interface(version:2,layout:\"x\",thread:2) { "
        "Component(id:\"a\",source:\"a\",phase:\"critical\") }",
        "Interface(version:2,layout:\"x\") {}",
        "Interface(version:2,layout:\"../x\") { "
        "Component(id:\"a\",source:\"a\",phase:\"critical\") }",
        "Interface(version:2,layout:\"/x\") { Component(id:\"a\",source:\"a\",phase:\"critical\") "
        "}",
        One("Component(id:\"a\",source:\"../a\",phase:\"critical\")"),
        One("Component(id:\"a\",source:\"./a\",phase:\"critical\")"),
        One("Component(id:\"a\",source:\"/a\",phase:\"critical\")"),
        One("Component(id:\"9bad\",source:\"a\",phase:\"critical\")"),
        One("Component(id:\"a\",source:\"a\",phase:\"unknown\")"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\",after:[\"a\"])"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\",after:[\"missing\"])"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\",after:\"a\")"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\",after:[true])"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\",after:[\"b\",\"b\"]) "
            "Component(id:\"b\",source:\"b\",phase:\"critical\")"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\",after:[\"b\"]) "
            "Component(id:\"b\",source:\"b\",phase:\"deferred\")"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\",after:[\"b\"]) "
            "Component(id:\"b\",source:\"b\",phase:\"critical\",after:[\"a\"])"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\") "
            "Component(id:\"a\",source:\"b\",phase:\"critical\")"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\") { Text(\"illegal child\") }"),
        One("Component(id:\"a\",source:\"a\",phase:\"critical\").width(4)"),
        One("Text(\"not a declaration\")"),
        One("Binding(name:\"x\",type:\"number\",initial:\"wrong\") "
            "Component(id:\"a\",source:\"a\",phase:\"critical\")"),
        One("Binding(name:\"x\",type:\"color\",initial:\"@token\") "
            "Component(id:\"a\",source:\"a\",phase:\"critical\")"),
        One("Binding(name:\"x\",type:\"bool\",initial:1) "
            "Component(id:\"a\",source:\"a\",phase:\"critical\")"),
        One("Binding(name:\"x\",type:\"string\",initial:$other) "
            "Component(id:\"a\",source:\"a\",phase:\"critical\")"),
        One("Binding(name:\"x\",type:\"resource\",initial:1) "
            "Component(id:\"a\",source:\"a\",phase:\"critical\")"),
        One("Binding(name:\"x\",type:\"string\",initial:\"a\") "
            "Binding(name:\"x\",type:\"string\",initial:\"b\") "
            "Component(id:\"a\",source:\"a\",phase:\"critical\")")};
    for (const auto &text : invalid) {
        Fails(CompileAttempt{text});
    }
    Fails(CompileAttempt{"Interface(version:2,layout:\"x\") {"}, LoadStage::Syntax);

    std::string many;
    for (int i = 0; i < 129; ++i) {
        many +=
            "Component(id:\"c" + std::to_string(i) + "\",source:\"c.prism\",phase:\"critical\")\n";
    }
    Fails(CompileAttempt{One(many)});
    Fails(CompileAttempt{std::string(kMaxLoadFileBytes + 1, ' ')});
}

void CheckLayoutAndBindings()
{
    const auto plan = Plan();
    const auto layout = PrepareLayout(layout_source, plan);
    assert(layout.Slots().size() == 3 && !layout.IsLegacy());
    assert(layout.Slots()[0].component == "first");
    assert(layout.Slots()[1].node_path == std::vector<std::size_t>{1});
    assert(layout.Body().Root().children[0].kind == Kind::Box);
    assert(layout.RetainedBytes() >= layout.Body().RetainedBytes());

    const std::vector<std::string> invalid{
        "HStack { Slot(component:\"first\") Slot(component:\"second\") }",
        "HStack { Slot(component:\"first\") Slot(component:\"second\") Slot(component:\"later\") "
        "Slot(component:\"first\") }",
        "HStack { Slot(component:\"first\") Slot(component:\"second\") Slot(component:\"unknown\") "
        "}",
        "HStack { Slot(component:\"first\",width:\"wrong\") Slot(component:\"second\") "
        "Slot(component:\"later\") }",
        "HStack { Slot(component:\"first\",width:8,width:9) Slot(component:\"second\") "
        "Slot(component:\"later\") }",
        "HStack { Slot(component:\"first\",unknown:9) Slot(component:\"second\") "
        "Slot(component:\"later\") }",
        "HStack { Slot(component:\"first\",component:\"second\") Slot(component:\"second\") "
        "Slot(component:\"later\") }",
        "HStack { Slot(component:$label) Slot(component:\"second\") Slot(component:\"later\") }",
        "HStack { Slot(component:\"first\") { Button(\"go\",\"action\") } "
        "Slot(component:\"second\") Slot(component:\"later\") }",
        "HStack { Slot(component:\"first\") { IconButton(\"play\",\"action\") } "
        "Slot(component:\"second\") Slot(component:\"later\") }",
        "HStack { Slot(component:\"first\") { Toggle(action:\"toggle\") } "
        "Slot(component:\"second\") Slot(component:\"later\") }",
        "HStack { Slot(component:\"first\") { Slot(component:\"second\") } "
        "Slot(component:\"later\") }",
        "HStack { Slot(component:\"first\") { Text($missing) } Slot(component:\"second\") "
        "Slot(component:\"later\") }"};
    for (const auto &text : invalid) {
        Fails(LayoutAttempt{plan, text});
    }
    Fails(OrdinaryAttempt{"Slot(component:\"first\")"});
    Fails(OrdinaryAttempt{std::string(interface_source)});

    const auto valid = Unit(
        "VStack { Text($label,foreground:$ink) Progress(value:$progress) Card(visible:$ready) }",
        "first");
    ValidatePreparedUnit(plan, "first", valid);
    ValidatePreparedUnit(plan, "later", Unit("Text($label)", "later"));
    Fails(ValidateAttempt{plan, "missing", valid});
    Fails(ValidateAttempt{plan, "second", valid});
    Fails(ValidateAttempt{plan, "later", Unit("Text($undeclared)", "later")});
    Fails(ValidateAttempt{plan, "first", Unit("Text($progress)", "first")});
    Fails(ValidateAttempt{plan, "first", Unit("Progress(value:$label)", "first")});
    Fails(ValidateAttempt{plan, "first", Unit("Text(\"x\",font:$ready)", "first")});
    Fails(ValidateAttempt{plan, "first", Unit("Image($label)", "first")});
}

std::vector<std::string> Texts(const PreparedNode &node)
{
    std::vector<std::string> out;
    for (const auto &property : node.properties) {
        if (property.id == DslProperty::Text) {
            out.push_back(std::get<std::string>(property.value));
        }
    }
    for (const auto &child : node.children) {
        auto nested = Texts(child);
        out.insert(out.end(), nested.begin(), nested.end());
    }
    return out;
}

struct ImageResolver {
    prism::contracts::ResourceId operator()(std::string_view uri) const
    {
        assert(uri == "checker.png");
        return {77};
    }
};

void CheckComposition()
{
    const auto plan = Plan();
    const auto layout = PrepareLayout(layout_source, plan);
    std::vector<PreparedUnit> units{
        {"first", Unit("VStack { Text(\"FIRST\") Image(\"checker.png\") Text($label) }", "first")},
        {"second", Unit("Text(\"SECOND\",foreground:\"@late_theme\")", "second")},
        {"later", Unit("Text(\"Not mounted yet\")", "later")}};
    const auto first = ComposeCritical(plan, layout, units);
    std::reverse(units.begin(), units.end());
    const auto reversed = ComposeCritical(plan, layout, units);
    assert(
        (Texts(first.Root()) == std::vector<std::string>{"FIRST", "SECOND", "Deferred pending"}));
    assert(Texts(first.Root()) == Texts(reversed.Root()));
    assert(first.Root().children[0].kind == Kind::Box);
    assert(first.Root().children[0].region == "first" && first.Root().children[0].region_mounted);
    assert(first.Root().children[2].region == "later" && !first.Root().children[2].region_mounted);
    assert(first.Root().children[0].children.size() == 1);
    assert(first.Root().children[1].children[0].theme_refs[0].name == "late_theme");
    assert(first.Images().size() == 1 && first.Images()[0].uri == "checker.png");
    assert(reversed.Images()[0].uri == "checker.png");
    const auto expected_bytes = plan.source_bytes + layout_source.size() +
                                units[1].prepared.SourceBytes() + units[2].prepared.SourceBytes();
    assert(first.SourceBytes() == expected_bytes && reversed.SourceBytes() == expected_bytes);
    const auto linked = LinkComponent(first, ImageResolver{});
    assert(linked.children[0].region == "first" && linked.children[0].region_mounted);
    assert(linked.children[2].region == "later" && !linked.children[2].region_mounted);
    const auto &image = linked.children[0].children[0].children[1];
    const auto &placeholder_image = linked.children[2].children[0].children[1];
    assert(std::get<prism::contracts::ResourceId>(image.properties[0].value).value == 77);
    assert(std::get<prism::contracts::ResourceId>(placeholder_image.properties[0].value).value ==
           77);
}

void CheckLimitsAndLegacy()
{
    const auto plan = Plan();
    const auto layout = PrepareLayout(layout_source, plan);
    const auto a = Unit("Text(\"A\")", "first");
    const auto b = Unit("Text(\"B\")", "second");
    Fails(ComposeAttempt{plan, layout, {{"first", a}}});
    Fails(ComposeAttempt{plan, layout, {{"first", a}, {"first", a}, {"second", b}}});
    Fails(ComposeAttempt{plan, PreparedLayout::Legacy(), {{"first", a}, {"second", b}}});

    std::string nodes = "VStack {";
    for (int i = 0; i < 4200; ++i) {
        nodes += "Text(\"n\")";
    }
    nodes += '}';
    Fails(ComposeAttempt{
        plan, layout, {{"first", Unit(nodes, "first")}, {"second", Unit(nodes, "second")}}});
    std::string deep = "Text(\"deep\")";
    for (int i = 0; i < 64; ++i) {
        deep = "VStack {" + deep + '}';
    }
    Fails(ComposeAttempt{plan, layout, {{"first", Unit(deep, "first")}, {"second", b}}});
    const auto effect = "VStack {Card(backdropBlur:1) Card(backdropBlur:1) Card(backdropBlur:1) "
                        "Card(backdropBlur:1) Card(backdropBlur:1)}";
    Fails(ComposeAttempt{
        plan, layout, {{"first", Unit(effect, "first")}, {"second", Unit(effect, "second")}}});

    const auto legacy = Plan("Text($undeclared)");
    assert(legacy.legacy && legacy.version == 1 && legacy.components.size() == 1);
    const auto master = PrepareComponent("Text($undeclared)", legacy.source);
    const std::vector<PreparedUnit> unit{{"master", master}};
    const auto composed = ComposeCritical(legacy, PreparedLayout::Legacy(), unit);
    assert(composed.NodeCount() == master.NodeCount() &&
           composed.SourceBytes() == master.SourceBytes());
    assert(composed.Root().bindings[0].name == "undeclared");

    std::string declarations, slots;
    std::vector<PreparedUnit> large;
    const std::string near_limit =
        "Text(\"large\")" +
        std::string(kMaxLoadFileBytes - std::string("Text(\"large\")").size(), ' ');
    for (int i = 0; i < 8; ++i) {
        const auto id = "c" + std::to_string(i);
        declarations +=
            "Component(id:\"" + id + "\",source:\"" + id + ".prism\",phase:\"critical\")";
        slots += "Slot(component:\"" + id + "\")";
        large.push_back({id, Unit(near_limit, id)});
    }
    const auto aggregate = Plan(One(declarations));
    const auto aggregate_layout = PrepareLayout("HStack {" + slots + '}', aggregate);
    Fails(ComposeAttempt{aggregate, aggregate_layout, std::move(large)});

    std::string many_tokens = "Text(\"a\",";
    for (int i = 0; i < 20000; ++i) {
        many_tokens += "width:1,";
    }
    many_tokens += "height:1)";
    Fails(CompileAttempt{std::move(many_tokens)}, LoadStage::Syntax);
}

void CheckRetention()
{
    const auto plan = Plan();
    auto lease_a = std::make_shared<int>(1);
    auto lease_layout = std::make_shared<int>(2);
    std::weak_ptr<int> weak_a = lease_a, weak_layout = lease_layout;
    std::optional<PreparedComponent> composed;
    {
        auto layout = PrepareLayout(layout_source, plan).WithRetention(lease_layout);
        std::vector<PreparedUnit> units{
            {"first", Unit("Text(\"A\")", "first").WithRetention(lease_a)},
            {"second", Unit("Text(\"B\")", "second")}};
        composed = ComposeCritical(plan, layout, units);
        lease_a.reset();
        lease_layout.reset();
    }
    assert(!weak_a.expired() && !weak_layout.expired());
    composed.reset();
    assert(weak_a.expired() && weak_layout.expired());
}
} // namespace

int main()
{
    CheckEntryRecognition();
    CheckPlan();
    CheckLayoutAndBindings();
    CheckComposition();
    CheckLimitsAndLegacy();
    CheckRetention();
}
