#include "prism/runtime/dsl_syntax.hpp"
#include "prism/compiler/error.hpp"
#include "prism/compiler/lexer.hpp"
#include <stdexcept>
#include <string>
#include <utility>

namespace prism::runtime {
namespace {
using compiler::Token;
using compiler::TokenType;

class SyntaxParser {
public:
    explicit SyntaxParser(std::vector<Token> tokens) : tokens_(std::move(tokens))
    {
    }

    SyntaxNode Parse()
    {
        auto root = ParseNode();
        Require(TokenType::EndOfFile, "unexpected input after root component");
        return root;
    }

private:
    const Token &Current() const
    {
        return tokens_.at(index_);
    }

    const Token &Next() const
    {
        return tokens_.at(index_ + 1);
    }

    bool Is(TokenType type) const
    {
        return Current().type == type;
    }

    bool Match(TokenType type)
    {
        if (!Is(type)) {
            return false;
        }
        ++index_;
        return true;
    }

    [[noreturn]] void Error(const std::string &message) const
    {
        throw compiler::CompilerError(Current().line, message);
    }

    Token Require(TokenType type, const std::string &message)
    {
        if (!Is(type)) {
            Error(message);
        }
        return tokens_[index_++];
    }

    SyntaxValue ParseValue(unsigned depth = 0)
    {
        if (++value_count_ > 65536) {
            Error("AST value count exceeds limit of 65536");
        }
        if (depth > 64) {
            Error("value nesting exceeds limit");
        }
        if (Is(TokenType::StringLiteral)) {
            return {{Require(TokenType::StringLiteral, "string expected").text}};
        }
        if (Is(TokenType::DollarIdentifier)) {
            auto token = Require(TokenType::DollarIdentifier, "binding expected");
            if (token.text.empty()) {
                Error("empty binding");
            }
            return {{BindingValue{token.text}}};
        }
        if (Is(TokenType::Identifier)) {
            auto token = Require(TokenType::Identifier, "identifier expected");
            if (token.text == "true") {
                return {{true}};
            }
            if (token.text == "false") {
                return {{false}};
            }
            return {{IdentifierValue{token.text}}};
        }
        if (Is(TokenType::NumberLiteral)) {
            auto token = Require(TokenType::NumberLiteral, "number expected");
            if (!token.text.empty() && token.text.front() == '#') {
                const auto digits = token.text.size() - 1;
                if (digits != 6 && digits != 8) {
                    Error("color must have 6 or 8 hex digits");
                }
                auto packed =
                    static_cast<std::uint32_t>(std::stoul(token.text.substr(1), nullptr, 16));
                if (digits == 6) {
                    packed = (packed << 8) | 0xff;
                }
                return {{ColorValue{packed}}};
            }
            return {{token.number_value}};
        }
        if (Match(TokenType::OpenBracket)) {
            SyntaxValue::List values;
            if (!Is(TokenType::CloseBracket)) {
                do {
                    values.push_back(ParseValue(depth + 1));
                } while (Match(TokenType::Comma) && !Is(TokenType::CloseBracket));
            }
            Require(TokenType::CloseBracket, "expected ']' after list");
            return {{std::move(values)}};
        }
        Error("expected a value");
    }

    std::vector<SyntaxArgument> ParseArguments()
    {
        std::vector<SyntaxArgument> result;
        if (!Match(TokenType::OpenParen)) {
            return result;
        }
        while (!Is(TokenType::CloseParen)) {
            if (Is(TokenType::EndOfFile)) {
                Error("expected ')' after arguments");
            }
            if (++argument_count_ > 65536) {
                Error("AST argument count exceeds limit of 65536");
            }
            SyntaxArgument arg;
            arg.line = Current().line;
            if (Is(TokenType::Identifier) && Next().type == TokenType::Colon) {
                arg.name = Require(TokenType::Identifier, "argument name expected").text;
                Require(TokenType::Colon, "expected ':' after argument name");
            }
            arg.value = ParseValue();
            result.push_back(std::move(arg));
            if (!Match(TokenType::Comma)) {
                break;
            }
        }
        Require(TokenType::CloseParen, "expected ')' after arguments");
        return result;
    }

    SyntaxNode ParseNode(unsigned depth = 0)
    {
        if (depth > 64 || ++node_count_ > 8192) {
            Error("component nesting or count exceeds limit");
        }
        auto name = Require(TokenType::Identifier, "expected component name");
        SyntaxNode node;
        node.name = std::move(name.text);
        node.line = name.line;
        node.arguments = ParseArguments();
        if (Match(TokenType::OpenBrace)) {
            while (!Is(TokenType::CloseBrace)) {
                if (Is(TokenType::EndOfFile)) {
                    Error("expected '}' after children");
                }
                node.children.push_back(ParseNode(depth + 1));
            }
            Require(TokenType::CloseBrace, "expected '}' after children");
        }
        while (Match(TokenType::Dot)) {
            auto modifier = Require(TokenType::Identifier, "expected modifier name");
            node.modifiers.push_back({std::move(modifier.text), ParseArguments(), modifier.line});
        }
        return node;
    }

    std::vector<Token> tokens_;
    std::size_t index_{0};
    std::size_t node_count_{0};
    std::size_t argument_count_{0};
    std::size_t value_count_{0};
};
} // namespace

SyntaxNode ParseSyntax(std::string_view source)
{
    return SyntaxParser(compiler::Lexer(std::string(source), true).Tokenize()).Parse();
}
} // namespace prism::runtime
