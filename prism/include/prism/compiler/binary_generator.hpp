#pragma once

#include "prism/compiler/ast.hpp"
#include "prism/compiler/binary_format.hpp"
#include <string>
#include <vector>
#include <memory>

namespace prism::compiler {

class BinaryGenerator {
public:
    BinaryGenerator() = default;

    std::vector<uint8_t> Generate(const std::shared_ptr<AstNode>& root);
    std::vector<uint8_t> GenerateTheme(const std::shared_ptr<AstNode>& root);
    bool WriteToFile(const std::shared_ptr<AstNode>& root, const std::string& output_path);

private:
    uint16_t FlattenNode(const std::shared_ptr<AstNode>& node, uint16_t parent_index);
    uint32_t AddString(const std::string& str);

    std::vector<PrismbNodeRecord> node_records_;
    std::string string_table_;
};

} // namespace prism::compiler
