#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include <cassert>
#include <stdexcept>
#include <string>
#include <variant>

int main()
{
    using namespace prism::runtime;
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
}
