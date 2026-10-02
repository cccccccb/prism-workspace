#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
void RejectStateSource(const char *source, const char *expected)
{
    try {
        prism::runtime::PrepareComponent(source);
    } catch (const prism::runtime::LoadFailure &failure) {
        assert(failure.Diagnostic().stage == prism::runtime::LoadStage::Semantic);
        if (std::string(failure.what()).find(expected) == std::string::npos) {
            std::fprintf(stderr, "source: %s\nexpected: %s\nactual: %s\n", source, expected,
                         failure.what());
        }
        assert(std::string(failure.what()).find(expected) != std::string::npos);
        return;
    }
    assert(false);
}

void CheckStateRules()
{
    using namespace prism::runtime;
    const auto prepared = PrepareComponent(R"(
        InteractionTarget(width:80,height:32,action:"go") {
            Visual(width:40,height:4,background:"@accent",scaleX:$baseScale) {
                Icon("play",foreground:"@accent")
                    .state(when:"hovered",scope:"target",foreground:#FFFFFFFF)
            }
                .state(when:"hovered",scope:"target",scaleX:1.15,background:"@hover")
                .state(when:"pressed",scope:"target",scaleX:0.94)
                .state(when:"captured",scope:"target",translateX:2)
                .state(when:"disabled",scope:"target",opacity:0.4)
                .state(when:"focused",scope:"target",translateY:1)
                .state(when:"focusVisible",scope:"target",scaleY:1.2)
                .transition(property:"scaleX",durationMs:120,easing:"linear")
        }
    )");
    assert(prepared.Root().kind == Kind::InteractionTarget);
    const auto &visual = prepared.Root().children.front();
    assert(visual.kind == Kind::Visual && visual.state_rules.size() == 6);
    assert(visual.state_rules[0].condition == StateCondition::Hovered);
    assert(visual.state_rules[0].properties ==
           (std::vector<PropertyAssignment>{{DslProperty::ScaleX, 1.15}}));
    assert(visual.state_rules[0].theme_refs ==
           (std::vector<ThemeRef>{{"hover", DslProperty::Background}}));
    assert(visual.bindings.size() == 1 && visual.bindings[0].name == "baseScale");
    assert(visual.theme_refs == (std::vector<ThemeRef>{{"accent", DslProperty::Background}}));
    assert(visual.children.front().state_rules.size() == 1);
    const auto linked = LinkComponent(prepared);
    assert(linked.children.front().state_rules == visual.state_rules);
    assert(linked.children.front().children.front().state_rules ==
           visual.children.front().state_rules);

    const auto plain = PrepareComponent("InteractionTarget { Visual(width:40,height:4) }");
    const auto ruled = PrepareComponent(
        "InteractionTarget { Visual(width:40,height:4)"
        ".state(when:\"hovered\",scope:\"target\",background:\"@hover\",scaleX:1.1) }");
    assert(ruled.RetainedBytes() >= plain.RetainedBytes() + sizeof(StateRule) +
                                        sizeof(PropertyAssignment) + sizeof(ThemeRef));
    // Separate declarations may augment the same condition on independent properties.
    const auto composed = ParseBlueprint(R"(
        InteractionTarget { Visual {
                Visual(translateX:2,originX:0,originY:1) { Text("Label") }
            }
            .state(when:"hovered",scope:"target",scaleX:1.1)
            .state(when:"hovered",scope:"target",opacity:0.8)
        }
    )");
    assert(composed.children.front().state_rules.size() == 2);

    RejectStateSource("Visual(width:40,height:4)", "Visual cannot be a component root");
    RejectStateSource("Card { Visual.state(when:\"hovered\",scope:\"target\",scaleX:1.1) }",
                      "state requires a Visual subtree inside an InteractionTarget");
    RejectStateSource("InteractionTarget { Icon(\"play\")"
                      ".state(when:\"hovered\",scope:\"target\",foreground:#FFFFFFFF) }",
                      "state requires a Visual subtree inside an InteractionTarget");
    RejectStateSource("InteractionTarget { Visual.state(scope:\"target\",scaleX:1.1) }",
                      "state requires when, scope");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",scaleX:1.1) }",
                      "state requires when, scope");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",scope:\"target\") }",
                      "state requires at least one property");
    RejectStateSource("InteractionTarget { Visual.state(when:\"unknownState\",scope:\"target\","
                      "scaleX:1.1) }",
                      "unknown state condition");
    RejectStateSource("InteractionTarget { Visual.state(when:$condition,scope:\"target\","
                      "scaleX:1.1) }",
                      "unknown state condition");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",scope:$scope,"
                      "scaleX:1.1) }",
                      "state scope must be the literal target");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",scope:\"parent\","
                      "scaleX:1.1) }",
                      "state scope must be the literal target");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",scope:\"target\","
                      "scaleX:$scale) }",
                      "state values require literals or theme references");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",scope:\"target\","
                      "width:40) }",
                      "state property not supported");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",when:\"pressed\","
                      "scope:\"target\",scaleX:1.1) }",
                      "duplicate state argument");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",scope:\"target\","
                      "scaleX:1.1,scaleX:1.2) }",
                      "duplicate state property");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",scope:\"target\","
                      "scaleX:1.1).state(when:\"hovered\",scope:\"target\",scaleX:1.2) }",
                      "duplicate state condition property");
    RejectStateSource("InteractionTarget { Visual.state(when:\"hovered\",scope:\"target\","
                      "scaleX:1.1).state(when:\"focusVisible\",scope:\"target\",scaleX:1.2) }",
                      "focus state property conflicts");
    RejectStateSource("InteractionTarget { Visual { Card(action:\"go\") } }",
                      "property not allowed");
    RejectStateSource("InteractionTarget { Visual { IconButton(\"play\",\"go\") } }",
                      "Visual subtree cannot declare action");
    RejectStateSource("InteractionTarget { Visual { Card(material:\"window\") } }",
                      "Visual subtree cannot declare material");
    RejectStateSource("InteractionTarget { Visual { Card(backdropBlur:0) } }",
                      "Visual subtree cannot declare backdropBlur");
    RejectStateSource("InteractionTarget { Visual { Card(inputShape:\"visible\") } }",
                      "Visual subtree cannot declare inputShape");
    RejectStateSource("InteractionTarget { Visual { InteractionTarget(action:\"go\") } }",
                      "Visual subtree cannot contain InteractionTarget");
    RejectStateSource("Card(scaleX:1.1)", "property not allowed");
    RejectStateSource("InteractionTarget { Visual(scaleX:0) }", "invalid numeric value");
    RejectStateSource("InteractionTarget { Visual(scaleY:9) }", "invalid numeric value");
    RejectStateSource("InteractionTarget { Visual(opacity:1.1) }", "invalid numeric value");
    RejectStateSource("InteractionTarget { Visual(originX:-0.1) }", "invalid numeric value");
    RejectStateSource("InteractionTarget { Visual(translateX:8193) }", "invalid numeric value");
    RejectStateSource("InteractionTarget { Visual.transition(property:\"originX\","
                      "durationMs:100,easing:\"linear\") }",
                      "property cannot transition");
}
} // namespace

int main()
{
    CheckStateRules();
    using namespace prism::runtime;
    namespace animation = prism::animation;
    auto syntax =
        ParseSyntax("Custom(size: 12, active: true, color: #123456, values: [1, false]) {\n"
                    "  Other($name).effect(2)\n}");
    assert(syntax.name == "Custom" && syntax.arguments.size() == 4);
    assert(std::holds_alternative<double>(syntax.arguments[0].value.data));
    assert(std::get<bool>(syntax.arguments[1].value.data));
    assert(std::get<ColorValue>(syntax.arguments[2].value.data).rgba == 0x123456ff);
    assert(std::get<SyntaxValue::List>(syntax.arguments[3].value.data).size() == 2);
    assert(syntax.children.size() == 1 && syntax.children[0].line == 2);
    assert(std::get<BindingValue>(syntax.children[0].arguments[0].value.data).name == "name");
    assert(syntax.children[0].modifiers[0].name == "effect");
    auto button = ParseBlueprint("Button(\"Launch\", \"app:launch\")");
    assert(button.properties.size() == 1 && button.properties[0].id == DslProperty::Action &&
           std::get<std::string>(button.properties[0].value) == "app:launch");
    assert(button.children.size() == 1 && button.children[0].properties.size() == 1 &&
           button.children[0].properties[0].id == DslProperty::Text &&
           std::get<std::string>(button.children[0].properties[0].value) == "Launch");

    const auto animated =
        PrepareComponent("VStack {\n"
                         "  Progress(value:$progress).transition(property:\"value\",durationMs:180,"
                         "easing:\"easeOutCubic\")\n"
                         "  Text(\"Status\",foreground:$ink).transition(property:\"foreground\","
                         "durationMs:0,easing:\"linear\")\n"
                         "}");
    assert(animated.Root().children[0].transitions.size() == 1);
    assert(animated.Root().children[0].transitions[0] ==
           (TransitionSpec{DslProperty::Value, 180, animation::Easing::EaseOutCubic}));
    assert(animated.Root().children[1].transitions[0] ==
           (TransitionSpec{DslProperty::Foreground, 0, animation::Easing::Linear}));
    const auto linked = LinkComponent(animated);
    assert(linked.children[0].transitions == animated.Root().children[0].transitions);
    assert(linked.children[1].transitions == animated.Root().children[1].transitions);
    const auto reordered = ParseBlueprint(
        "Icon(\"music\",foreground:#FFFFFFFF)"
        ".transition(easing:\"easeInCubic\",durationMs:10000,property:\"foreground\")");
    assert(reordered.transitions ==
           (std::vector<TransitionSpec>{
               {DslProperty::Foreground, 10000, animation::Easing::EaseInCubic}}));
    const auto plain = PrepareComponent("Progress(value:$progress)");
    const auto with_transition =
        PrepareComponent("Progress(value:$progress).transition(property:\"value\",durationMs:180,"
                         "easing:\"easeOutCubic\")");
    assert(with_transition.RetainedBytes() >= plain.RetainedBytes() + sizeof(TransitionSpec));

    auto rejects = [](auto parse, const char *source, const char *expected) {
        try {
            parse(source);
        } catch (const std::runtime_error &error) {
            assert(std::string(error.what()).find(expected) != std::string::npos);
            return;
        }
        assert(false);
    };
    rejects([](auto s) { ParseSyntax(s); }, "Custom(a: 1) Tail()", "after root");
    rejects([](auto s) { ParseSyntax(s); }, "Custom(a: @)", "unexpected character");
    rejects([](auto s) { ParseSyntax(s); }, "Custom(a: #123)", "color must have");
    rejects([](auto s) { ParseBlueprint(s); }, "Text(\"x\", font: \"huge\")", "wrong value type");
    rejects([](auto s) { ParseBlueprint(s); }, "Text(\"x\", font: 12, font: 13)",
            "duplicate property");
    rejects([](auto s) { ParseBlueprint(s); }, "Text(\"x\", mystery: 1)", "unknown property");
    rejects([](auto s) { ParseBlueprint(s); }, "Unknown()", "unsupported client DSL component");
    rejects([](auto s) { ParseBlueprint(s); }, "Image(\"a\") { Text(\"b\") }",
            "cannot have children");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:\"value\",durationMs:180)",
            "transition requires property, durationMs and easing");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(\"value\",durationMs:180,easing:\"linear\")",
            "unknown transition argument");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:\"value\",property:\"value\",durationMs:180,"
            "easing:\"linear\")",
            "duplicate transition argument");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:\"value\",durationMs:180,easing:\"linear\")"
            ".transition(property:\"value\",durationMs:90,easing:\"easeInCubic\")",
            "duplicate transition property");
    rejects([](auto s) { ParseBlueprint(s); },
            "Text(\"x\").transition(property:\"value\",durationMs:180,easing:\"linear\")",
            "property cannot transition");
    rejects([](auto s) { ParseBlueprint(s); },
            "Button(\"x\").transition(property:\"foreground\",durationMs:180,"
            "easing:\"linear\")",
            "property cannot transition");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:\"background\",durationMs:180,easing:\"linear\")",
            "property cannot transition");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:$name,durationMs:180,easing:\"linear\")",
            "property cannot transition");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:\"value\",durationMs:1.5,easing:\"linear\")",
            "transition durationMs must be an integer");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:\"value\",durationMs:10001,easing:\"linear\")",
            "transition durationMs must be an integer");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:\"value\",durationMs:$delay,easing:\"linear\")",
            "transition durationMs must be an integer");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:\"value\",durationMs:100,easing:\"spring\")",
            "unknown transition easing");
    rejects([](auto s) { ParseBlueprint(s); },
            "Progress().transition(property:\"value\",durationMs:100,easing:easeInCubic)",
            "unknown transition easing");

    try {
        PrepareComponent("VStack {\n Progress().transition(property:\"value\",durationMs:-1,"
                         "easing:\"linear\")\n}");
        assert(false);
    } catch (const LoadFailure &error) {
        assert(error.Diagnostic().stage == LoadStage::Semantic);
        assert(error.Diagnostic().line == 2);
    }
}
