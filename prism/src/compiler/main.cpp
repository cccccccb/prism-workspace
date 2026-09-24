#include "prism/compiler/lexer.hpp"
#include "prism/compiler/parser.hpp"
#include "prism/compiler/binary_generator.hpp"
#include "prism/core/logging.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: prism-compiler <input.prism> <output.prismb>\n";
        return 1;
    }

    std::string input_path = argv[1];
    std::string output_path = argv[2];

    PRISM_LOG_INFO("COMPILER", "Compiling Prism DSL: '%s' -> '%s'...", input_path.c_str(), output_path.c_str());

    std::ifstream file(input_path);
    if (!file) {
        PRISM_LOG_ERROR("COMPILER", "Cannot open input file: %s", input_path.c_str());
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    try {
        // 1. Lexical Analysis
        prism::compiler::Lexer lexer(source);
        auto tokens = lexer.Tokenize();

        // 2. Syntax Parsing to AST
        prism::compiler::Parser parser(tokens);
        auto ast = parser.Parse();
        if (!ast) {
            PRISM_LOG_ERROR("COMPILER", "Failed to parse AST (empty root)");
            return 1;
        }

        // 3. AOT Binary Code Generation
        prism::compiler::BinaryGenerator generator;
        if (!generator.WriteToFile(ast, output_path)) {
            return 1;
        }

        PRISM_LOG_INFO("COMPILER", "Compilation completed successfully!");
        return 0;
    } catch (const std::exception& e) {
        PRISM_LOG_ERROR("COMPILER", "Compilation error: %s", e.what());
        return 1;
    }
}
