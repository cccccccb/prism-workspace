#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace prism::runtime {

struct BindingValue { std::string name; };
struct IdentifierValue { std::string name; };
struct ColorValue { std::uint32_t rgba; };
struct SyntaxValue {
    using List = std::vector<SyntaxValue>;
    std::variant<double, bool, std::string, BindingValue, IdentifierValue, ColorValue, List> data;
};
struct SyntaxArgument {
    std::string name; // Empty for a positional argument.
    SyntaxValue value;
    int line{1};
};
struct SyntaxModifier {
    std::string name;
    std::vector<SyntaxArgument> arguments;
    int line{1};
};
struct SyntaxNode {
    std::string name;
    std::vector<SyntaxArgument> arguments;
    std::vector<SyntaxNode> children;
    std::vector<SyntaxModifier> modifiers;
    int line{1};
};

// Syntax only: component and property names are opaque to this parser.
SyntaxNode ParseSyntax(std::string_view source);
} // namespace prism::runtime
