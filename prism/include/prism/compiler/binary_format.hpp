#pragma once

#include <cstdint>

namespace prism::compiler {

constexpr uint32_t PRISMB_MAGIC = 0x50525342; // "PRSB" (Prism Binary)
constexpr uint16_t PRISMB_VERSION = 2;

enum class BinaryNodeType : uint16_t {
    Unknown  = 0,
    VStack   = 1,
    HStack   = 2,
    Text     = 3,
    Button   = 4,
    Slider   = 5,
    Skeleton = 6,
    Icon     = 7
};

#pragma pack(push, 1)
struct PrismbHeader {
    uint32_t magic;                 // PRISMB_MAGIC
    uint16_t version;               // PRISMB_VERSION
    uint16_t node_count;            // Number of nodes in binary table
    uint32_t string_table_offset;   // Byte offset to string table
    uint32_t string_table_size;     // Size of string table in bytes
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
