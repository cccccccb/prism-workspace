#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include <cassert>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

int main()
{
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
