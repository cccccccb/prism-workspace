#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace prism::compiler {

class CompilerError : public std::runtime_error {
public:
    CompilerError(int line, std::string message)
        : std::runtime_error("DSL line " + std::to_string(line) + ": " + message), line_(line),
          message_(std::move(message))
    {
    }

    int Line() const noexcept
    {
        return line_;
    }

    const std::string &Message() const noexcept
    {
        return message_;
    }

private:
    int line_;
    std::string message_;
};

} // namespace prism::compiler
