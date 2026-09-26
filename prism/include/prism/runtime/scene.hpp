#pragma once

#include "prism/contracts/display_list.hpp"
#include "prism/runtime/property.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <map>

namespace prism::runtime {

enum class Kind { Row, Column, Box, Text, Image };
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
    std::vector<PropertyAssignment> properties;
    std::vector<PropertyBinding> bindings;
    std::uint64_t allowed_properties{UINT64_MAX};
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
    bool SetBinding(std::string_view name, PropertyValue value);
    bool SetProperty(contracts::NodeId id, DslProperty property, PropertyValue value);
    bool SetViewport(contracts::LogicalSize size);
    bool SetBackground(contracts::NodeId id, contracts::Color color);
    bool ImageReady(contracts::ResourceId image, contracts::LogicalSize intrinsic_size);
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
    void ApplyCachedProperty(Node& node, DslProperty property, const PropertyValue& value);
    PropertyValue CurrentProperty(const Node& node, DslProperty property) const;
    void Layout(Node& node, contracts::LogicalRect bounds);
    void Paint(const Node& node, contracts::DisplayList& list) const;
    std::optional<std::string> Hit(const Node& node, contracts::LogicalPoint point) const;

    std::unique_ptr<Node> root_;
    ShapeText shaper_;
    contracts::ResourceId font_{};
    contracts::LogicalSize viewport_{};
    std::vector<Node*> nodes_;
    struct BindingTarget { Node* node; DslProperty property; };
    std::unordered_map<std::string, std::vector<BindingTarget>> bindings_;
    Dirty dirty_{Dirty::Layout | Dirty::Paint};
    std::uint64_t generation_{0};
};

} // namespace prism::runtime
