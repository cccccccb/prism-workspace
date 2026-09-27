#pragma once

#include "prism/compiler/binary_format.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace prism::compiler {

struct AstModifier {
    std::string name; // "blur", "cornerRadius", "padding", "springAnimation", "acrylic", "glow",
                      // "springOnHover"
    std::vector<float> float_args;
    std::vector<std::string> str_args;
    std::unordered_map<std::string, double> named_floats;
    std::unordered_map<std::string, std::string> named_strings;
};

struct AstNode {
    BinaryNodeType type{BinaryNodeType::Unknown};
    std::string name;
    std::string slot_binding; // Non-empty if e.g. "$track_title"

    // Properties
    std::string text_value;
    std::string action_value;
    std::string icon_value;
    float numeric_value{0.0f};
    float spacing{8.0f};

    // Generic Named Properties (for Decoration and extensible components)
    std::unordered_map<std::string, double> number_props;
    std::unordered_map<std::string, std::string> string_props;

    // Modifiers list
    std::vector<AstModifier> modifiers;

    // Children
    std::vector<std::shared_ptr<AstNode>> children;

    void AddChild(std::shared_ptr<AstNode> child)
    {
        if (child) {
            children.push_back(std::move(child));
        }
    }
};

} // namespace prism::compiler
