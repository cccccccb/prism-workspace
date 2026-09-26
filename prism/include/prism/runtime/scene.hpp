#pragma once

#include "prism/contracts/display_list.hpp"
#include "prism/contracts/surface_effect.hpp"
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
struct RenderTree;

enum class Kind { Row, Column, Box, Text, Image, Icon, IconButton, Progress, Toggle, Separator };
struct Style {
    double width{0};  // automatic: intrinsic leaves, remaining-space containers
    double height{0}; // automatic; Card fills containers and measures text
    double padding{0};
    double spacing{0};
    double radius{0};
    contracts::Color background{0, 0, 0, 0};
    contracts::Color foreground{255, 255, 255, 255};
    double font_size{16};
    bool clip{false};
    std::string align{"stretch"};
    std::string justify{"start"};
    std::string anchor{"fill"};
    std::string overflow{"visible"};
    double flex{0};
    double inset{0};
    double padding_x{-1}, padding_y{-1};
    double border_width{0};
    contracts::Color border_color{255, 255, 255, 0};
    double shadow_blur{0}, shadow_y{0};
    contracts::Color shadow_color{0, 0, 0, 0};
    double inner_shadow_blur{0};
    contracts::Color inner_shadow_color{0, 0, 0, 0};
    double backdrop_blur{0};
    contracts::ImageFit image_fit{contracts::ImageFit::Fill};
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
    bool AcceptsBinding(std::string_view name, const PropertyValue& value) const;
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
    std::vector<contracts::SurfaceEffectRegion> SurfaceEffects() const;
    const std::vector<contracts::SurfaceInputRegion>& InputRegions() const;
    bool SetPointer(contracts::LogicalPoint point);
    bool FocusNext();
    std::optional<std::string> FocusedAction() const;

private:
    struct Node;
    std::unique_ptr<Node> MakeNode(Blueprint blueprint);
    Node* Find(contracts::NodeId id) const;
    void ApplyCachedProperty(Node& node, DslProperty property, const PropertyValue& value);
    PropertyValue CurrentProperty(const Node& node, DslProperty property) const;
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
    std::unique_ptr<RenderTree> render_tree_;
    Node* hovered_{nullptr};
    Node* focused_{nullptr};
    mutable bool input_dirty_{true};
    mutable std::vector<contracts::SurfaceInputRegion> input_regions_;
};

} // namespace prism::runtime
