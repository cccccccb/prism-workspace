#include "prism/compiler/binary_generator.hpp"
#include "prism/core/types.hpp"
#include "prism/core/logging.hpp"
#include <fstream>
#include <cstring>

namespace prism::compiler {

uint32_t BinaryGenerator::AddString(const std::string& str) {
    if (str.empty()) return 0xFFFFFFFF;
    uint32_t offset = static_cast<uint32_t>(string_table_.size());
    string_table_.append(str);
    string_table_.push_back('\0'); // Null terminator
    return offset;
}

uint16_t BinaryGenerator::FlattenNode(const std::shared_ptr<AstNode>& node, uint16_t parent_index) {
    if (!node) return 0xFFFF;

    uint16_t my_index = static_cast<uint16_t>(node_records_.size());
    PrismbNodeRecord record{};
    record.node_type = node->type;
    record.parent_index = parent_index;
    record.first_child_index = 0xFFFF;
    record.next_sibling_index = 0xFFFF;
    record.spacing = node->spacing;
    record.width = 0.0f;
    record.height = 0.0f;

    // Slot binding
    if (!node->slot_binding.empty()) {
        record.slot_id = core::HashSlot(node->slot_binding);
    } else {
        record.slot_id = 0;
    }

    // Strings
    record.name_offset = AddString(node->name);
    record.text_offset = AddString(node->text_value);
    record.action_offset = AddString(node->action_value);
    record.icon_offset = AddString(node->icon_value);

    // Numeric value defaults (font, shimmer, scale)
    if (node->type == BinaryNodeType::Skeleton) {
        record.height = node->numeric_value; // shimmer speed
    } else if (node->type == BinaryNodeType::Icon) {
        record.width = node->numeric_value;  // scale
    } else if (node->type == BinaryNodeType::Text) {
        record.height = node->numeric_value; // font size
    }

    // Process Modifiers
    for (const auto& mod : node->modifiers) {
        if (mod.name == "blur") {
            if (!mod.float_args.empty()) record.blur_radius = mod.float_args[0];
            if (mod.float_args.size() > 1) record.blur_passes = static_cast<int16_t>(mod.float_args[1]);
            else record.blur_passes = 4;
        } else if (mod.name == "cornerRadius") {
            if (!mod.float_args.empty()) record.corner_radius = mod.float_args[0];
        } else if (mod.name == "padding") {
            if (!mod.float_args.empty()) record.padding = mod.float_args[0];
        } else if (mod.name == "springAnimation") {
            if (!mod.float_args.empty()) record.spring_damping = mod.float_args[0];
            if (mod.float_args.size() > 1) record.spring_stiffness = mod.float_args[1];
        }
    }

    // Reserve space for this node record
    node_records_.push_back(record);

    // Recursively flatten children and link siblings (Left-Child Right-Sibling)
    uint16_t prev_child_index = 0xFFFF;
    for (size_t i = 0; i < node->children.size(); ++i) {
        uint16_t child_index = FlattenNode(node->children[i], my_index);
        if (i == 0) {
            node_records_[my_index].first_child_index = child_index;
        } else if (prev_child_index != 0xFFFF) {
            node_records_[prev_child_index].next_sibling_index = child_index;
        }
        prev_child_index = child_index;
    }

    return my_index;
}

std::vector<uint8_t> BinaryGenerator::Generate(const std::shared_ptr<AstNode>& root) {
    node_records_.clear();
    string_table_.clear();

    if (root) {
        FlattenNode(root, 0xFFFF);
    }

    PrismbHeader header{};
    header.magic = PRISMB_MAGIC;
    header.version = PRISMB_VERSION;
    header.node_count = static_cast<uint16_t>(node_records_.size());
    header.string_table_offset = static_cast<uint32_t>(sizeof(PrismbHeader) + node_records_.size() * sizeof(PrismbNodeRecord));
    header.string_table_size = static_cast<uint32_t>(string_table_.size());

    size_t total_size = header.string_table_offset + header.string_table_size;
    std::vector<uint8_t> buffer(total_size);

    // 1. Copy Header
    std::memcpy(buffer.data(), &header, sizeof(PrismbHeader));

    // 2. Copy Node Records
    size_t records_size = node_records_.size() * sizeof(PrismbNodeRecord);
    if (records_size > 0) {
        std::memcpy(buffer.data() + sizeof(PrismbHeader), node_records_.data(), records_size);
    }

    // 3. Copy String Table
    if (header.string_table_size > 0) {
        std::memcpy(buffer.data() + header.string_table_offset, string_table_.data(), header.string_table_size);
    }

    return buffer;
}

bool BinaryGenerator::WriteToFile(const std::shared_ptr<AstNode>& root, const std::string& output_path) {
    auto binary = Generate(root);
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        PRISM_LOG_ERROR("COMPILER", "Failed to open output file: %s", output_path.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(binary.data()), binary.size());
    PRISM_LOG_INFO("COMPILER", "Successfully emitted AOT binary '%s' (%zu bytes, %zu nodes)",
                   output_path.c_str(), binary.size(), node_records_.size());
    return true;
}

} // namespace prism::compiler
