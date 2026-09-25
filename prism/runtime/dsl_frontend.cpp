#include "prism/runtime/dsl_frontend.hpp"
#include "prism/compiler/lexer.hpp"
#include "prism/compiler/parser.hpp"
#include <algorithm>
#include <stdexcept>

namespace prism::runtime {
namespace {
Blueprint Convert(const compiler::AstNode& ast) {
    Blueprint out;
    using Type = compiler::BinaryNodeType;
    switch (ast.type) {
        case Type::HStack: out.kind = Kind::Row; break;
        case Type::VStack: out.kind = Kind::Column; break;
        case Type::Card: out.kind = Kind::Box; break;
        case Type::Text: out.kind = Kind::Text; break;
        case Type::Button: out.kind = Kind::Box; break;
        default: throw std::runtime_error("Unsupported client DSL component: " + ast.name);
    }
    out.text = ast.text_value;
    out.slot = ast.slot_binding;
    out.action = ast.action_value;
    out.style.spacing = ast.spacing;
    if (auto it = ast.number_props.find("width"); it != ast.number_props.end()) out.style.width = it->second;
    if (auto it = ast.number_props.find("height"); it != ast.number_props.end()) out.style.height = it->second;
    if (auto it = ast.number_props.find("font"); it != ast.number_props.end()) out.style.font_size = it->second;
    for (const auto& modifier : ast.modifiers) {
        if (modifier.name == "padding" && !modifier.float_args.empty()) out.style.padding = modifier.float_args.front();
        else if (modifier.name == "cornerRadius" && !modifier.float_args.empty()) out.style.radius = modifier.float_args.front();
        else if (modifier.name == "clip") out.style.clip = true;
        else throw std::runtime_error("Unsupported client DSL modifier: " + modifier.name);
    }
    for (const auto& child : ast.children) out.children.push_back(Convert(*child));
    if (ast.type == Type::Button) {
        Blueprint label;
        label.kind = Kind::Text;
        label.text = out.text;
        label.slot = out.slot;
        out.text.clear();
        out.slot.clear();
        out.children.push_back(std::move(label));
    }
    return out;
}
} // namespace
Blueprint ParseBlueprint(std::string_view source) {
    compiler::Lexer lexer{std::string(source)};
    auto ast = compiler::Parser(lexer.Tokenize()).Parse();
    if (!ast) throw std::runtime_error("Empty DSL document");
    return Convert(*ast);
}
} // namespace prism::runtime
