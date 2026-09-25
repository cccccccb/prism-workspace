#pragma once

#include "prism/contracts/display_list.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::runtime {

enum class Kind { Row, Column, Box, Text };
enum class Dirty : std::uint8_t { None = 0, Layout = 1, Paint = 2, Composite = 4 };
constexpr Dirty operator|(Dirty a, Dirty b) {
    return static_cast<Dirty>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr bool Has(Dirty value, Dirty bit) {
    return (static_cast<unsigned>(value) & static_cast<unsigned>(bit)) != 0;
}

struct Style {
    double width{0};  // 0 means fill the available width
    double height{0}; // 0 means intrinsic height for text, otherwise fill
    double padding{0};
    double spacing{0};
    double radius{0};
    contracts::Color background{0, 0, 0, 0};
    contracts::Color foreground{255, 255, 255, 255};
    double font_size{16};
    bool clip{false};
};

struct Blueprint {
    Kind kind{Kind::Box};
    Style style{};
    std::string text;
    std::string slot;
    std::string action;
    std::vector<Blueprint> children;
};

// Text shaping belongs to the client. The renderer only receives glyph IDs.
struct ShapedText {
    std::vector<contracts::GlyphPlacement> glyphs;
    double width{0};
    double height{0};
};
using ShapeText = std::function<ShapedText(std::string_view, double)>;

class Scene {
public:
    explicit Scene(Blueprint root, ShapeText shaper, contracts::ResourceId font = {});
    ~Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    bool SetSlot(std::string_view name, std::string value);
    bool SetViewport(contracts::LogicalSize size);
    bool SetBackground(contracts::NodeId id, contracts::Color color);
    std::optional<contracts::DisplayList> Build(contracts::WindowId window);
    std::optional<std::string> ActionAt(contracts::LogicalPoint point) const;
    Dirty PendingDirty() const { return dirty_; }
    std::uint64_t Generation() const { return generation_; }
    contracts::NodeId RootId() const;
    contracts::LogicalRect Bounds(contracts::NodeId id) const;

private:
    struct Node;
    std::unique_ptr<Node> MakeNode(Blueprint blueprint);
    Node* Find(contracts::NodeId id) const;
    void Layout(Node& node, contracts::LogicalRect bounds);
    void Paint(const Node& node, contracts::DisplayList& list) const;
    std::optional<std::string> Hit(const Node& node, contracts::LogicalPoint point) const;

    std::unique_ptr<Node> root_;
    ShapeText shaper_;
    contracts::ResourceId font_{};
    contracts::LogicalSize viewport_{};
    std::vector<Node*> nodes_;
    Dirty dirty_{Dirty::Layout | Dirty::Paint};
    std::uint64_t generation_{0};
};

} // namespace prism::runtime
