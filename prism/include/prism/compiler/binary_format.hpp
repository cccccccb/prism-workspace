#pragma once

#include <cstdint>

namespace prism::compiler {

constexpr uint32_t PRISMB_MAGIC = 0x50525342; // "PRSB" (Prism Binary UI AST)
constexpr uint32_t PRISMB_THEME_MAGIC = 0x5448454D; // "THEM" (Prism Tiling Decoration Theme)
constexpr uint16_t PRISMB_VERSION = 3;

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
    DropZone         = 13,
    Motion           = 14,
    MotionFold       = 15,
    MotionFullscreen = 16,
    MotionSplitMove  = 17,
    MotionFocus      = 18,
    Toggle           = 19,
    TextInput        = 20,
    ProgressBar      = 21,
    Card             = 22,
    Spacer           = 23,
    Badge            = 24,
    ZStack           = 25,
    Desktop          = 26,
    TopBar           = 27,
    Dock             = 28,
    AppGroup         = 29
};

#pragma pack(push, 1)
enum class MotionEngineType : uint8_t {
    None        = 0,
    Spring      = 1,
    CubicBezier = 2
};

struct PrismbMotionCurveRecord {
    uint8_t  engine_type;      // MotionEngineType (0=None, 1=Spring, 2=CubicBezier)
    uint8_t  flags;            // Bit0: clip_content, Bit1: fade_content, Bit2: smart_gaps_collapse
    uint16_t duration_ms;      // duration in milliseconds
    float    param0;           // Spring: damping / Bezier: x1
    float    param1;           // Spring: stiffness / Bezier: y1
    float    param2;           // Bezier: x2
    float    param3;           // Bezier: y2
};

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

    // Motion & Kinetic Physics
    PrismbMotionCurveRecord motion_fold;
    PrismbMotionCurveRecord motion_fullscreen;
    PrismbMotionCurveRecord motion_split_move;
    PrismbMotionCurveRecord motion_focus;
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
    uint32_t       glow_color;
    float          glow_radius;
    uint32_t       tint_color;
    float          hover_scale;

    // String Table Offsets (0xFFFFFFFF if none)
    uint32_t       name_offset;
    uint32_t       text_offset;
    uint32_t       action_offset;
    uint32_t       icon_offset;
};
#pragma pack(pop)

} // namespace prism::compiler
