#pragma once

#include "prism/compiler/ast.hpp"
#include "prism/compiler/lexer.hpp"
#include <memory>
#include <vector>

namespace prism::compiler {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    std::shared_ptr<AstNode> Parse();

private:
    const Token &Peek() const;
    const Token &Previous() const;
    bool IsAtEnd() const;
    const Token &Advance();
    bool Check(TokenType type) const;
    bool Match(TokenType type);
    const Token &Consume(TokenType type, const std::string &err_msg);

    std::shared_ptr<AstNode> ParseNode();
    AstModifier ParseModifier();

    BinaryNodeType ResolveNodeType(const std::string &name);

    std::vector<Token> tokens_;
    size_t current_{0};
};

} // namespace prism::compiler
