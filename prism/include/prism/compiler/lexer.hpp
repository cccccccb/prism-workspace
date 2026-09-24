#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace prism::compiler {

enum class TokenType {
    Identifier,       // e.g. VStack, Text, blur
    DollarIdentifier, // e.g. $track_title
    StringLiteral,    // e.g. "Media Library"
    NumberLiteral,    // e.g. 18, 0.75
    Dot,              // .
    OpenBrace,        // {
    CloseBrace,       // }
    OpenParen,        // (
    CloseParen,       // )
    Colon,            // :
    Comma,            // ,
    EndOfFile
};

struct Token {
    TokenType type;
    std::string text;
    double number_value{0.0};
    int line{1};
};

class Lexer {
public:
    explicit Lexer(std::string source);

    std::vector<Token> Tokenize();

private:
    char Peek() const;
    char Advance();
    bool IsAtEnd() const;
    void SkipWhitespaceAndComments();

    Token ScanIdentifierOrKeyword();
    Token ScanDollarIdentifier();
    Token ScanString();
    Token ScanNumber();
    Token ScanHexColor();

    std::string source_;
    size_t cursor_{0};
    int line_{1};
};

} // namespace prism::compiler
