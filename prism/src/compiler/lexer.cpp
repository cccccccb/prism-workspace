#include "prism/compiler/lexer.hpp"
#include "prism/compiler/error.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace prism::compiler {

Lexer::Lexer(std::string source, bool strict) : source_(std::move(source)), strict_(strict)
{
}

char Lexer::Peek() const
{
    if (IsAtEnd()) {
        return '\0';
    }
    return source_[cursor_];
}

char Lexer::Advance()
{
    if (IsAtEnd()) {
        return '\0';
    }
    char c = source_[cursor_++];
    if (c == '\n') {
        line_++;
    }
    return c;
}

bool Lexer::IsAtEnd() const
{
    return cursor_ >= source_.size();
}

void Lexer::SkipWhitespaceAndComments()
{
    while (!IsAtEnd()) {
        char c = Peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            Advance();
        } else if (c == '/' && cursor_ + 1 < source_.size() && source_[cursor_ + 1] == '/') {
            // Line comment: skip until newline
            while (!IsAtEnd() && Peek() != '\n') {
                Advance();
            }
        } else {
            break;
        }
    }
}

Token Lexer::ScanIdentifierOrKeyword()
{
    size_t start = cursor_;
    while (!IsAtEnd() && (std::isalnum(Peek()) || Peek() == '_')) {
        Advance();
    }
    std::string text = source_.substr(start, cursor_ - start);
    return Token{TokenType::Identifier, text, 0.0, line_};
}

Token Lexer::ReadRootIdentifier()
{
    SkipWhitespaceAndComments();
    if (IsAtEnd() || (!std::isalpha(static_cast<unsigned char>(Peek())) && Peek() != '_')) {
        throw CompilerError(line_, "expected root component identifier");
    }
    return ScanIdentifierOrKeyword();
}

Token Lexer::ScanDollarIdentifier()
{
    Advance(); // Consume '$'
    size_t start = cursor_;
    while (!IsAtEnd() && (std::isalnum(Peek()) || Peek() == '_')) {
        Advance();
    }
    std::string text = source_.substr(start, cursor_ - start);
    return Token{TokenType::DollarIdentifier, text, 0.0, line_};
}

Token Lexer::ScanString()
{
    const int start_line = line_;
    Advance(); // Consume leading '"'
    std::string text;
    while (!IsAtEnd() && Peek() != '"') {
        if (Peek() == '\\' && cursor_ + 1 < source_.size()) {
            Advance(); // skip '\'
            char next = Advance();
            if (next == 'n') {
                text += '\n';
            } else if (next == 'r') {
                text += '\r';
            } else if (next == 't') {
                text += '\t';
            } else {
                text += next;
            }
        } else {
            text += Advance();
        }
    }
    if (IsAtEnd() && strict_) {
        throw CompilerError(line_, "unterminated string");
    }
    if (!IsAtEnd()) {
        Advance(); // Consume closing '"'
    }
    return Token{TokenType::StringLiteral, text, 0.0, start_line};
}

Token Lexer::ScanNumber()
{
    size_t start = cursor_;
    const bool negative = Peek() == '-';
    if (negative) {
        Advance();
    }
    const auto numeric_start = cursor_;
    if (Peek() == '0' && cursor_ + 1 < source_.size() &&
        (source_[cursor_ + 1] == 'x' || source_[cursor_ + 1] == 'X')) {
        Advance(); // '0'
        Advance(); // 'x'
        while (!IsAtEnd() && std::isxdigit(Peek())) {
            Advance();
        }
        if (strict_ && cursor_ == numeric_start + 2) {
            throw CompilerError(line_, "invalid hex number");
        }
        std::string text = source_.substr(start, cursor_ - start);
        double val =
            static_cast<double>(std::strtoull(text.c_str() + (negative ? 1 : 0), nullptr, 16)) *
            (negative ? -1 : 1);
        return Token{TokenType::NumberLiteral, text, val, line_};
    }
    while (!IsAtEnd() && (std::isdigit(Peek()) || Peek() == '.')) {
        Advance();
    }
    std::string text = source_.substr(start, cursor_ - start);
    if (strict_ && std::count(text.begin(), text.end(), '.') > 1) {
        throw CompilerError(line_, "invalid number");
    }
    double val = std::strtod(text.c_str(), nullptr);
    return Token{TokenType::NumberLiteral, text, val, line_};
}

Token Lexer::ScanHexColor()
{
    Advance(); // Consume '#'
    size_t start = cursor_;
    while (!IsAtEnd() && std::isxdigit(Peek())) {
        Advance();
    }
    std::string hex_str = source_.substr(start, cursor_ - start);
    if (strict_ && hex_str.size() != 6 && hex_str.size() != 8) {
        throw CompilerError(line_, "color must have 6 or 8 hex digits");
    }
    uint32_t val = 0;
    if (hex_str.size() == 6) {
        val = (static_cast<uint32_t>(std::strtoul(hex_str.c_str(), nullptr, 16)) << 8) | 0xFF;
    } else if (hex_str.size() == 8) {
        val = static_cast<uint32_t>(std::strtoul(hex_str.c_str(), nullptr, 16));
    } else {
        val = static_cast<uint32_t>(std::strtoul(hex_str.c_str(), nullptr, 16));
    }
    return Token{TokenType::NumberLiteral, "#" + hex_str, static_cast<double>(val), line_};
}

std::vector<Token> Lexer::Tokenize()
{
    std::vector<Token> tokens;
    while (!IsAtEnd()) {
        SkipWhitespaceAndComments();
        if (IsAtEnd()) {
            break;
        }
        if (strict_ && tokens.size() >= 65535) {
            throw CompilerError(line_, "DSL token count exceeds limit of 65536");
        }

        char c = Peek();
        if (std::isalpha(c) || c == '_') {
            tokens.push_back(ScanIdentifierOrKeyword());
        } else if (c == '$') {
            tokens.push_back(ScanDollarIdentifier());
        } else if (c == '"') {
            tokens.push_back(ScanString());
        } else if (c == '#') {
            tokens.push_back(ScanHexColor());
        } else if (std::isdigit(c) || (c == '-' && cursor_ + 1 < source_.size() &&
                                       std::isdigit(source_[cursor_ + 1]))) {
            tokens.push_back(ScanNumber());
        } else {
            Advance();
            switch (c) {
            case '.':
                tokens.push_back(Token{TokenType::Dot, ".", 0.0, line_});
                break;
            case '{':
                tokens.push_back(Token{TokenType::OpenBrace, "{", 0.0, line_});
                break;
            case '}':
                tokens.push_back(Token{TokenType::CloseBrace, "}", 0.0, line_});
                break;
            case '(':
                tokens.push_back(Token{TokenType::OpenParen, "(", 0.0, line_});
                break;
            case ')':
                tokens.push_back(Token{TokenType::CloseParen, ")", 0.0, line_});
                break;
            case '[':
                tokens.push_back(Token{TokenType::OpenBracket, "[", 0.0, line_});
                break;
            case ']':
                tokens.push_back(Token{TokenType::CloseBracket, "]", 0.0, line_});
                break;
            case ':':
                tokens.push_back(Token{TokenType::Colon, ":", 0.0, line_});
                break;
            case ',':
                tokens.push_back(Token{TokenType::Comma, ",", 0.0, line_});
                break;
            default:
                if (strict_) {
                    throw CompilerError(line_, std::string("unexpected character '") + c + "'");
                }
                break;
            }
        }
    }
    tokens.push_back(Token{TokenType::EndOfFile, "", 0.0, line_});
    return tokens;
}

} // namespace prism::compiler
