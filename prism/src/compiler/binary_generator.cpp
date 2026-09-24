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

    // Numeric value defaults (font, shimmer, scale, progress, toggle, spacer)
    if (node->type == BinaryNodeType::Skeleton) {
        record.height = node->numeric_value; // shimmer speed
    } else if (node->type == BinaryNodeType::Icon) {
        record.width = node->numeric_value;  // scale
    } else if (node->type == BinaryNodeType::Text) {
        record.height = node->numeric_value; // font size
    } else if (node->type == BinaryNodeType::Toggle) {
        record.width = node->numeric_value;  // initial state (0.0 or 1.0)
    } else if (node->type == BinaryNodeType::ProgressBar) {
        record.width = node->numeric_value;  // initial progress (0.0 .. 1.0)
    } else if (node->type == BinaryNodeType::Spacer) {
        record.width = node->numeric_value;  // length
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
        } else if (mod.name == "acrylic") {
            float blur = 20.0f;
            int16_t passes = 4;
            uint32_t tint = 0x221E2028;
            if (!mod.float_args.empty()) blur = mod.float_args[0];
            else if (mod.named_floats.count("blur")) blur = static_cast<float>(mod.named_floats.at("blur"));
            if (mod.float_args.size() > 1) passes = static_cast<int16_t>(mod.float_args[1]);
            else if (mod.named_floats.count("passes")) passes = static_cast<int16_t>(mod.named_floats.at("passes"));
            if (mod.named_floats.count("tint")) tint = static_cast<uint32_t>(mod.named_floats.at("tint"));
            else if (mod.float_args.size() > 2) tint = static_cast<uint32_t>(mod.float_args[2]);

            record.blur_radius = blur;
            record.blur_passes = passes;
            record.tint_color = tint;
        } else if (mod.name == "glow") {
            float radius = 12.0f;
            uint32_t color = 0x007AFFE6;
            if (!mod.float_args.empty()) radius = mod.float_args[0];
            else if (mod.named_floats.count("radius")) radius = static_cast<float>(mod.named_floats.at("radius"));
            if (mod.named_floats.count("color")) color = static_cast<uint32_t>(mod.named_floats.at("color"));
            else if (mod.float_args.size() > 1) color = static_cast<uint32_t>(mod.float_args[1]);

            record.glow_radius = radius;
            record.glow_color = color;
        } else if (mod.name == "springOnHover") {
            float scale = 1.05f;
            float damping = 0.8f;
            if (!mod.float_args.empty()) scale = mod.float_args[0];
            else if (mod.named_floats.count("scale")) scale = static_cast<float>(mod.named_floats.at("scale"));
            if (mod.float_args.size() > 1) damping = mod.float_args[1];
            else if (mod.named_floats.count("damping")) damping = static_cast<float>(mod.named_floats.at("damping"));

            record.hover_scale = scale;
            record.spring_damping = damping;
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

std::vector<uint8_t> BinaryGenerator::GenerateTheme(const std::shared_ptr<AstNode>& root) {
    PrismbThemeHeader header{};
    header.magic = PRISMB_THEME_MAGIC;
    header.version = PRISMB_VERSION;

    // Default parameters
    std::string theme_name = "DefaultTilingGlass";
    if (root && !root->text_value.empty()) {
        theme_name = root->text_value;
    }
    std::strncpy(header.theme_name, theme_name.c_str(), sizeof(header.theme_name) - 1);

    header.inner_gap = 10;
    header.outer_gap = 12;
    header.smart_gaps = 1;

    header.border_width = 2.0f;
    header.border_color_focused = 0x007AFFE6;
    header.border_color_unfocused = 0xFFFFFF1E;
    header.top_rim_specular = 0xFFFFFF32;
    header.corner_radius = 12.0f;

    header.bg_focused = 0x141822E6;
    header.bg_unfocused = 0x0E1016D9;
    header.blur_radius = 28.0f;
    header.blur_passes = 4;

    header.header_height = 32.0f;
    header.show_header = 1;
    header.header_bg_focused = 0x1C212EF0;
    header.header_bg_unfocused = 0x12141CDC;
    header.title_color_focused = 0xFFFFFFFF;
    header.title_color_unfocused = 0xA0A5AFB4;
    header.show_tiling_controls = 1;

    header.drop_fill_color = 0x007AFF3C;
    header.drop_border_color = 0x007AFFE6;
    header.drop_border_width = 2.0f;

    // Default Kinetic Motion Records
    header.motion_fold.engine_type = static_cast<uint8_t>(MotionEngineType::Spring);
    header.motion_fold.flags = 0x03; // clip_content (0x1) | fade_content (0x2)
    header.motion_fold.duration_ms = 250;
    header.motion_fold.param0 = 0.82f;   // damping
    header.motion_fold.param1 = 240.0f;  // stiffness

    header.motion_fullscreen.engine_type = static_cast<uint8_t>(MotionEngineType::CubicBezier);
    header.motion_fullscreen.flags = 0x04; // smart_gaps_collapse (0x4)
    header.motion_fullscreen.duration_ms = 280;
    header.motion_fullscreen.param0 = 0.16f; // x1
    header.motion_fullscreen.param1 = 1.00f; // y1
    header.motion_fullscreen.param2 = 0.30f; // x2
    header.motion_fullscreen.param3 = 1.00f; // y2

    header.motion_split_move.engine_type = static_cast<uint8_t>(MotionEngineType::Spring);
    header.motion_split_move.flags = 0x00;
    header.motion_split_move.duration_ms = 200;
    header.motion_split_move.param0 = 0.76f;   // damping
    header.motion_split_move.param1 = 260.0f;  // stiffness

    header.motion_focus.engine_type = static_cast<uint8_t>(MotionEngineType::CubicBezier);
    header.motion_focus.flags = 0x00;
    header.motion_focus.duration_ms = 150;
    header.motion_focus.param0 = 0.25f; // x1
    header.motion_focus.param1 = 0.10f; // y1
    header.motion_focus.param2 = 0.25f; // x2
    header.motion_focus.param3 = 1.00f; // y2

    if (root) {
        for (const auto& child : root->children) {
            if (child->name == "gaps") {
                if (child->number_props.count("inner")) header.inner_gap = static_cast<int16_t>(child->number_props.at("inner"));
                if (child->number_props.count("outer")) header.outer_gap = static_cast<int16_t>(child->number_props.at("outer"));
                if (child->number_props.count("smart")) header.smart_gaps = static_cast<uint8_t>(child->number_props.at("smart"));
            } else if (child->name == "border") {
                if (child->number_props.count("width")) header.border_width = static_cast<float>(child->number_props.at("width"));
                if (child->number_props.count("focused")) header.border_color_focused = static_cast<uint32_t>(child->number_props.at("focused"));
                if (child->number_props.count("unfocused")) header.border_color_unfocused = static_cast<uint32_t>(child->number_props.at("unfocused"));
                if (child->number_props.count("specular")) header.top_rim_specular = static_cast<uint32_t>(child->number_props.at("specular"));
                if (child->number_props.count("cornerRadius")) header.corner_radius = static_cast<float>(child->number_props.at("cornerRadius"));
            } else if (child->name == "backdrop") {
                if (child->number_props.count("focused")) header.bg_focused = static_cast<uint32_t>(child->number_props.at("focused"));
                if (child->number_props.count("unfocused")) header.bg_unfocused = static_cast<uint32_t>(child->number_props.at("unfocused"));
                if (child->number_props.count("blur")) header.blur_radius = static_cast<float>(child->number_props.at("blur"));
                if (child->number_props.count("passes")) header.blur_passes = static_cast<int16_t>(child->number_props.at("passes"));
            } else if (child->name == "header") {
                if (child->number_props.count("height")) header.header_height = static_cast<float>(child->number_props.at("height"));
                if (child->number_props.count("show")) header.show_header = static_cast<uint8_t>(child->number_props.at("show"));
                if (child->number_props.count("focused")) header.header_bg_focused = static_cast<uint32_t>(child->number_props.at("focused"));
                if (child->number_props.count("unfocused")) header.header_bg_unfocused = static_cast<uint32_t>(child->number_props.at("unfocused"));
                if (child->number_props.count("titleFocused")) header.title_color_focused = static_cast<uint32_t>(child->number_props.at("titleFocused"));
                if (child->number_props.count("titleUnfocused")) header.title_color_unfocused = static_cast<uint32_t>(child->number_props.at("titleUnfocused"));
                if (child->number_props.count("controls")) header.show_tiling_controls = static_cast<uint8_t>(child->number_props.at("controls"));
            } else if (child->name == "dropZone") {
                if (child->number_props.count("fill")) header.drop_fill_color = static_cast<uint32_t>(child->number_props.at("fill"));
                if (child->number_props.count("border")) header.drop_border_color = static_cast<uint32_t>(child->number_props.at("border"));
                if (child->number_props.count("width")) header.drop_border_width = static_cast<float>(child->number_props.at("width"));
            } else if (child->name == "motion" || child->name == "animation") {
                for (const auto& m_sub : child->children) {
                    if (m_sub->name == "fold") {
                        if (m_sub->string_props.count("engine")) {
                            header.motion_fold.engine_type = (m_sub->string_props.at("engine") == "bezier") ?
                                static_cast<uint8_t>(MotionEngineType::CubicBezier) : static_cast<uint8_t>(MotionEngineType::Spring);
                        }
                        if (m_sub->number_props.count("duration")) header.motion_fold.duration_ms = static_cast<uint16_t>(m_sub->number_props.at("duration"));
                        if (m_sub->number_props.count("damping")) header.motion_fold.param0 = static_cast<float>(m_sub->number_props.at("damping"));
                        if (m_sub->number_props.count("stiffness")) header.motion_fold.param1 = static_cast<float>(m_sub->number_props.at("stiffness"));
                    } else if (m_sub->name == "fullscreen" || m_sub->name == "monocle") {
                        if (m_sub->string_props.count("engine")) {
                            header.motion_fullscreen.engine_type = (m_sub->string_props.at("engine") == "spring") ?
                                static_cast<uint8_t>(MotionEngineType::Spring) : static_cast<uint8_t>(MotionEngineType::CubicBezier);
                        }
                        if (m_sub->number_props.count("duration")) header.motion_fullscreen.duration_ms = static_cast<uint16_t>(m_sub->number_props.at("duration"));
                        if (m_sub->number_props.count("bezier_x1")) header.motion_fullscreen.param0 = static_cast<float>(m_sub->number_props.at("bezier_x1"));
                        if (m_sub->number_props.count("bezier_y1")) header.motion_fullscreen.param1 = static_cast<float>(m_sub->number_props.at("bezier_y1"));
                        if (m_sub->number_props.count("bezier_x2")) header.motion_fullscreen.param2 = static_cast<float>(m_sub->number_props.at("bezier_x2"));
                        if (m_sub->number_props.count("bezier_y2")) header.motion_fullscreen.param3 = static_cast<float>(m_sub->number_props.at("bezier_y2"));
                    } else if (m_sub->name == "splitMove" || m_sub->name == "split_move") {
                        if (m_sub->string_props.count("engine")) {
                            header.motion_split_move.engine_type = (m_sub->string_props.at("engine") == "bezier") ?
                                static_cast<uint8_t>(MotionEngineType::CubicBezier) : static_cast<uint8_t>(MotionEngineType::Spring);
                        }
                        if (m_sub->number_props.count("duration")) header.motion_split_move.duration_ms = static_cast<uint16_t>(m_sub->number_props.at("duration"));
                        if (m_sub->number_props.count("damping")) header.motion_split_move.param0 = static_cast<float>(m_sub->number_props.at("damping"));
                        if (m_sub->number_props.count("stiffness")) header.motion_split_move.param1 = static_cast<float>(m_sub->number_props.at("stiffness"));
                    } else if (m_sub->name == "focus") {
                        if (m_sub->string_props.count("engine")) {
                            header.motion_focus.engine_type = (m_sub->string_props.at("engine") == "spring") ?
                                static_cast<uint8_t>(MotionEngineType::Spring) : static_cast<uint8_t>(MotionEngineType::CubicBezier);
                        }
                        if (m_sub->number_props.count("duration")) header.motion_focus.duration_ms = static_cast<uint16_t>(m_sub->number_props.at("duration"));
                        if (m_sub->number_props.count("bezier_x1")) header.motion_focus.param0 = static_cast<float>(m_sub->number_props.at("bezier_x1"));
                        if (m_sub->number_props.count("bezier_y1")) header.motion_focus.param1 = static_cast<float>(m_sub->number_props.at("bezier_y1"));
                        if (m_sub->number_props.count("bezier_x2")) header.motion_focus.param2 = static_cast<float>(m_sub->number_props.at("bezier_x2"));
                        if (m_sub->number_props.count("bezier_y2")) header.motion_focus.param3 = static_cast<float>(m_sub->number_props.at("bezier_y2"));
                    }
                }
            }
        }
    }

    std::vector<uint8_t> buffer(sizeof(PrismbThemeHeader));
    std::memcpy(buffer.data(), &header, sizeof(PrismbThemeHeader));
    return buffer;
}

std::vector<uint8_t> BinaryGenerator::Generate(const std::shared_ptr<AstNode>& root) {
    if (root && root->type == BinaryNodeType::TilingDecoration) {
        return GenerateTheme(root);
    }

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
    if (root && root->type == BinaryNodeType::TilingDecoration) {
        PRISM_LOG_INFO("COMPILER", "Successfully emitted AOT Theme binary '%s' (%zu bytes)",
                       output_path.c_str(), binary.size());
    } else {
        PRISM_LOG_INFO("COMPILER", "Successfully emitted AOT UI binary '%s' (%zu bytes, %zu nodes)",
                       output_path.c_str(), binary.size(), node_records_.size());
    }
    return true;
}

} // namespace prism::compiler
