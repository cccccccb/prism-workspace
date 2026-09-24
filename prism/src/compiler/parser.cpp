#include "prism/compiler/parser.hpp"
#include "prism/core/logging.hpp"
#include <stdexcept>

namespace prism::compiler {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::Peek() const {
    return tokens_[current_];
}

const Token& Parser::Previous() const {
    return tokens_[current_ - 1];
}

bool Parser::IsAtEnd() const {
    return Peek().type == TokenType::EndOfFile;
}

const Token& Parser::Advance() {
    if (!IsAtEnd()) current_++;
    return Previous();
}

bool Parser::Check(TokenType type) const {
    if (IsAtEnd()) return false;
    return Peek().type == type;
}

bool Parser::Match(TokenType type) {
    if (Check(type)) {
        Advance();
        return true;
    }
    return false;
}

const Token& Parser::Consume(TokenType type, const std::string& err_msg) {
    if (Check(type)) return Advance();
    PRISM_LOG_ERROR("PARSER", "Syntax error at line %d: %s (got '%s')", Peek().line, err_msg.c_str(), Peek().text.c_str());
    throw std::runtime_error(err_msg);
}

BinaryNodeType Parser::ResolveNodeType(const std::string& name) {
    if (name == "VStack")           return BinaryNodeType::VStack;
    if (name == "HStack")           return BinaryNodeType::HStack;
    if (name == "Text")             return BinaryNodeType::Text;
    if (name == "Button")           return BinaryNodeType::Button;
    if (name == "Slider")           return BinaryNodeType::Slider;
    if (name == "Skeleton")         return BinaryNodeType::Skeleton;
    if (name == "Icon")             return BinaryNodeType::Icon;
    if (name == "TilingDecoration") return BinaryNodeType::TilingDecoration;
    if (name == "gaps")             return BinaryNodeType::Gaps;
    if (name == "border")           return BinaryNodeType::Border;
    if (name == "backdrop")         return BinaryNodeType::Backdrop;
    if (name == "header")           return BinaryNodeType::Header;
    if (name == "dropZone")         return BinaryNodeType::DropZone;
    return BinaryNodeType::Unknown;
}

std::shared_ptr<AstNode> Parser::Parse() {
    if (IsAtEnd()) return nullptr;
    return ParseNode();
}

std::shared_ptr<AstNode> Parser::ParseNode() {
    const Token& tag_token = Consume(TokenType::Identifier, "Expected component identifier (e.g. VStack, Text, TilingDecoration)");
    auto node = std::make_shared<AstNode>();
    node->name = tag_token.text;
    node->type = ResolveNodeType(tag_token.text);

    // 1. Parse Arguments in ( ... )
    if (Match(TokenType::OpenParen)) {
        while (!Check(TokenType::CloseParen) && !IsAtEnd()) {
            std::string label;
            if (Check(TokenType::Identifier) && (current_ + 1 < tokens_.size()) && tokens_[current_ + 1].type == TokenType::Colon) {
                label = Advance().text;
                Consume(TokenType::Colon, "Expected ':' after argument label");
            }

            if (Check(TokenType::DollarIdentifier)) {
                // Reactive slot binding, e.g. $track_title
                node->slot_binding = Advance().text;
            } else if (Check(TokenType::StringLiteral)) {
                std::string s = Advance().text;
                if (!label.empty()) node->string_props[label] = s;
                if (label == "action") node->action_value = s;
                else if (label == "icon") node->icon_value = s;
                else if (node->text_value.empty()) node->text_value = s;
                else node->action_value = s;
            } else if (Check(TokenType::NumberLiteral)) {
                double num = Advance().number_value;
                if (!label.empty()) node->number_props[label] = num;
                if (label == "spacing") node->spacing = static_cast<float>(num);
                else if (label == "font") node->numeric_value = static_cast<float>(num);
                else if (label == "scale") node->numeric_value = static_cast<float>(num);
                else if (label == "shimmer") node->numeric_value = static_cast<float>(num);
                else node->numeric_value = static_cast<float>(num);
            } else if (Check(TokenType::Identifier) && (Peek().text == "true" || Peek().text == "false")) {
                bool b = (Advance().text == "true");
                double num = b ? 1.0 : 0.0;
                if (!label.empty()) node->number_props[label] = num;
                node->numeric_value = static_cast<float>(num);
            } else {
                Advance(); // Skip unknown
            }

            if (!Match(TokenType::Comma)) break;
        }
        Consume(TokenType::CloseParen, "Expected ')' after arguments");
    }

    // 2. Parse Children in { ... }
    if (Match(TokenType::OpenBrace)) {
        while (!Check(TokenType::CloseBrace) && !IsAtEnd()) {
            auto child = ParseNode();
            if (child) node->AddChild(child);
        }
        Consume(TokenType::CloseBrace, "Expected '}' after component body");
    }

    // 3. Parse Modifiers: .blur(...) .cornerRadius(...)
    while (Match(TokenType::Dot)) {
        node->modifiers.push_back(ParseModifier());
    }

    return node;
}

AstModifier Parser::ParseModifier() {
    const Token& name_tok = Consume(TokenType::Identifier, "Expected modifier name after '.'");
    AstModifier mod;
    mod.name = name_tok.text;

    if (Match(TokenType::OpenParen)) {
        while (!Check(TokenType::CloseParen) && !IsAtEnd()) {
            if (Check(TokenType::Identifier) && tokens_[current_ + 1].type == TokenType::Colon) {
                Advance(); // label
                Consume(TokenType::Colon, "Expected ':'");
            }

            if (Check(TokenType::NumberLiteral)) {
                mod.float_args.push_back(static_cast<float>(Advance().number_value));
            } else if (Check(TokenType::StringLiteral)) {
                mod.str_args.push_back(Advance().text);
            } else {
                Advance();
            }

            if (!Match(TokenType::Comma)) break;
        }
        Consume(TokenType::CloseParen, "Expected ')' after modifier arguments");
    }

    return mod;
}

} // namespace prism::compiler
