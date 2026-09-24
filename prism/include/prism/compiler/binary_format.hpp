#pragma once

#include <cstdint>

namespace prism::compiler {

constexpr uint32_t PRISMB_MAGIC = 0x50525342; // "PRSB" (Prism Binary UI AST)
constexpr uint32_t PRISMB_THEME_MAGIC = 0x5448454D; // "THEM" (Prism Tiling Decoration Theme)
constexpr uint16_t PRISMB_VERSION = 2;

enum class BinaryNodeType : uint16_t {
    Unknown          = 0,
    VStack           = 1,
    HStack           = 2,
    Text             = 3,
    Button           = 4,
    Slider           = 5,
    Skeleton         = 6,
    Icon             = 7,
    TilingDecoration = 8,
    Gaps             = 9,
    Border           = 10,
    Backdrop         = 11,
    Header           = 12,
    DropZone         = 13
};

#pragma pack(push, 1)
struct PrismbHeader {
    uint32_t magic;                 // PRISMB_MAGIC
    uint16_t version;               // PRISMB_VERSION
    uint16_t node_count;            // Number of nodes in binary table
    uint32_t string_table_offset;   // Byte offset to string table
    uint32_t string_table_size;     // Size of string table in bytes
};

struct PrismbThemeHeader {
    uint32_t magic;                 // PRISMB_THEME_MAGIC
    uint16_t version;               // PRISMB_VERSION
    char     theme_name[32];

    // Gaps
    int16_t  inner_gap;
    int16_t  outer_gap;
    uint8_t  smart_gaps;

    // Border
    float    border_width;
    uint32_t border_color_focused;
    uint32_t border_color_unfocused;
    uint32_t top_rim_specular;
    float    corner_radius;

    // Backdrop
    uint32_t bg_focused;
    uint32_t bg_unfocused;
    float    blur_radius;
    int16_t  blur_passes;

    // Header
    float    header_height;
    uint8_t  show_header;
    uint32_t header_bg_focused;
    uint32_t header_bg_unfocused;
    uint32_t title_color_focused;
    uint32_t title_color_unfocused;
    uint8_t  show_tiling_controls;

    // DropZone
    uint32_t drop_fill_color;
    uint32_t drop_border_color;
    float    drop_border_width;
};

/**
 * @brief Left-Child Right-Sibling (LCRS) contiguous binary node record
 */
struct PrismbNodeRecord {
    BinaryNodeType node_type;
    uint16_t       parent_index;        // 0xFFFF if root
    uint16_t       first_child_index;   // 0xFFFF if no children
    uint16_t       next_sibling_index;  // 0xFFFF if no next sibling
    uint32_t       slot_id;             // HashSlot for data-binding (0 if unbound)
    
    // Geometry & Layout
    float          width;
    float          height;
    float          spacing;

    // Stacked Modifiers
    float          blur_radius;
    int16_t        blur_passes;
    float          corner_radius;
    float          padding;
    float          spring_damping;
    float          spring_stiffness;

    // String Table Offsets (0xFFFFFFFF if none)
    uint32_t       name_offset;
    uint32_t       text_offset;
    uint32_t       action_offset;
    uint32_t       icon_offset;
};
#pragma pack(pop)

} // namespace prism::compiler
