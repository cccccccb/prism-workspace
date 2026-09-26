#include "prism/runtime/scene.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/scene_snapshot.hpp"
#include "prism/runtime/layout_engine.hpp"
#include "prism/runtime/render_tree.hpp"
#include "prism/runtime/display_list_builder.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace prism::runtime {

struct Scene::Node {
    contracts::NodeId id{};
    Kind kind{Kind::Box};
    Style style{};
    std::map<DslProperty, PropertyValue> properties;
    std::uint64_t allowed_properties{UINT64_MAX};
    std::string text;
    std::string action;
    contracts::ResourceId image{};
    contracts::LogicalSize intrinsic_size{};
    bool image_ready{false};
    contracts::LogicalRect bounds{};
    ShapedText shaped{};
    std::uint64_t revision{1};
    std::vector<std::unique_ptr<Node>> children;
};

namespace {
bool Inside(contracts::LogicalRect r, contracts::LogicalPoint p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height;
}
bool ValidSize(contracts::LogicalSize s) {
    return std::isfinite(s.width) && std::isfinite(s.height) && s.width > 0 && s.height > 0 &&
           s.width <= 16384 && s.height <= 16384;
}
bool ValidPropertyValue(DslProperty id, const PropertyValue& value) {
    const auto* spec = FindProperty(id);
    if (!spec) return false;
    switch (spec->stored_type) {
        case StoredValueType::Number: {
            const auto* number = std::get_if<double>(&value);
            return number && std::isfinite(*number) && *number >= spec->min_value &&
                *number <= spec->max_value && (spec->allow_zero || *number != 0);
        }
        case StoredValueType::Color: return std::holds_alternative<contracts::Color>(value);
        case StoredValueType::Boolean: return std::holds_alternative<bool>(value);
        case StoredValueType::String: return std::holds_alternative<std::string>(value);
        case StoredValueType::Resource: return std::holds_alternative<contracts::ResourceId>(value) &&
            static_cast<bool>(std::get<contracts::ResourceId>(value));
    }
    return false;
}
} // namespace

Scene::~Scene() = default;

Scene::Scene(Blueprint root, ShapeText shaper, contracts::ResourceId font)
    : shaper_(std::move(shaper)), font_(font) {
    if (!shaper_) throw std::invalid_argument("Scene requires a text shaper");
    root_ = MakeNode(std::move(root));
}

std::unique_ptr<Scene::Node> Scene::MakeNode(Blueprint blueprint) {
    auto node = std::make_unique<Node>();
    if (nodes_.size() >= UINT32_MAX) throw std::length_error("Scene node limit");
    node->id = {static_cast<std::uint32_t>(nodes_.size()), 1};
    Node* raw = node.get();
    nodes_.push_back(raw);
    node->kind = blueprint.kind;
    node->allowed_properties = blueprint.allowed_properties;
    for (auto& property : blueprint.properties) {
        if (!ValidPropertyValue(property.id, property.value) ||
            !(node->allowed_properties & PropertyBit(property.id)))
            throw std::invalid_argument("Invalid Blueprint property");
        node->properties[property.id] = property.value;
        ApplyCachedProperty(*node, property.id, property.value);
    }
    for (auto& binding : blueprint.bindings) {
        if (binding.name.empty() || !(node->allowed_properties & PropertyBit(binding.target)))
            throw std::invalid_argument("Invalid Blueprint binding");
        bindings_[binding.name].push_back({raw, binding.target});
    }
    for (auto& child : blueprint.children) node->children.push_back(MakeNode(std::move(child)));
    return node;
}

void Scene::ApplyCachedProperty(Node& node, DslProperty id, const PropertyValue& value) {
    switch (id) {
        case DslProperty::Width: node.style.width = std::get<double>(value); break;
        case DslProperty::Height: node.style.height = std::get<double>(value); break;
        case DslProperty::Font: node.style.font_size = std::get<double>(value); break;
        case DslProperty::Spacing: node.style.spacing = std::get<double>(value); break;
        case DslProperty::Padding: node.style.padding = std::get<double>(value); break;
        case DslProperty::Radius: node.style.radius = std::get<double>(value); break;
        case DslProperty::Background: node.style.background = std::get<contracts::Color>(value); break;
        case DslProperty::Foreground: node.style.foreground = std::get<contracts::Color>(value); break;
        case DslProperty::Clip: node.style.clip = std::get<bool>(value); break;
        case DslProperty::Text: node.text = std::get<std::string>(value); break;
        case DslProperty::Action: node.action = std::get<std::string>(value); break;
        case DslProperty::Source:
            node.image = std::get<contracts::ResourceId>(value);
            node.image_ready = false;
            node.intrinsic_size = {};
            break;
    }
}

PropertyValue Scene::CurrentProperty(const Node& node, DslProperty id) const {
    switch (id) {
        case DslProperty::Width: return node.style.width;
        case DslProperty::Height: return node.style.height;
        case DslProperty::Font: return node.style.font_size;
        case DslProperty::Spacing: return node.style.spacing;
        case DslProperty::Padding: return node.style.padding;
        case DslProperty::Radius: return node.style.radius;
        case DslProperty::Background: return node.style.background;
        case DslProperty::Foreground: return node.style.foreground;
        case DslProperty::Clip: return node.style.clip;
        case DslProperty::Text: return node.text;
        case DslProperty::Action: return node.action;
        case DslProperty::Source: return node.image;
    }
    return {};
}

Scene::Node* Scene::Find(contracts::NodeId id) const {
    if (!id || id.index >= nodes_.size()) return nullptr;
    Node* node = nodes_[id.index];
    return node->id == id ? node : nullptr;
}

contracts::NodeId Scene::RootId() const { return root_->id; }
contracts::LogicalRect Scene::Bounds(contracts::NodeId id) const {
    Node* node = Find(id);
    return node ? node->bounds : contracts::LogicalRect{};
}

bool Scene::SetSlot(std::string_view name, std::string value) {
    return SetBinding(name, std::move(value));
}

bool Scene::SetBinding(std::string_view name, PropertyValue value) {
    auto it = bindings_.find(std::string(name));
    if (it == bindings_.end()) return false;
    for (const auto& target : it->second)
        if (!ValidPropertyValue(target.property, value)) return false;
    bool changed = false;
    for (const auto& target : it->second)
        changed = SetProperty(target.node->id, target.property, value) || changed;
    return changed;
}

bool Scene::SetProperty(contracts::NodeId id, DslProperty property, PropertyValue value) {
    Node* node = Find(id);
    if (!node || !(node->allowed_properties & PropertyBit(property)) ||
        !ValidPropertyValue(property, value)) return false;
    if (CurrentProperty(*node, property) == value) return false;
    node->properties[property] = value;
    ApplyCachedProperty(*node, property, value);
    const Dirty affected = FindProperty(property)->affects;
    dirty_ = dirty_ | affected;
    if (affected != Dirty::None) ++node->revision;
    return true;
}

bool Scene::SetViewport(contracts::LogicalSize size) {
    if (!ValidSize(size)) return false;
    if (viewport_.width != size.width || viewport_.height != size.height) {
        viewport_ = size;
        dirty_ = dirty_ | Dirty::Layout | Dirty::Paint;
    }
    return true;
}

bool Scene::SetBackground(contracts::NodeId id, contracts::Color color) {
    return SetProperty(id, DslProperty::Background, color);
}

bool Scene::ImageReady(contracts::ResourceId image, contracts::LogicalSize intrinsic_size) {
    if (!image || !ValidSize(intrinsic_size)) return false;
    bool changed = false;
    for (Node* node : nodes_) {
        if (node->kind != Kind::Image || node->image != image) continue;
        if (!node->image_ready || node->intrinsic_size.width != intrinsic_size.width ||
            node->intrinsic_size.height != intrinsic_size.height) {
            const bool affects_layout = node->style.width <= 0 || node->style.height <= 0;
            node->intrinsic_size = intrinsic_size;
            node->image_ready = true;
            ++node->revision;
            dirty_ = dirty_ | Dirty::Paint | (affects_layout ? Dirty::Layout : Dirty::None);
            changed = true;
        }
    }
    return changed;
}

std::optional<contracts::DisplayList> Scene::Build(contracts::WindowId window) {
    if (!window || !ValidSize(viewport_) || dirty_ == Dirty::None) return std::nullopt;
    SceneSnapshot snapshot;
    snapshot.root = root_->id;
    snapshot.nodes.reserve(nodes_.size());
    for (const Node* node : nodes_) {
        SnapshotNode item;
        item.id = node->id;
        item.kind = node->kind;
        item.style = node->style;
        item.text = node->text;
        item.image = node->image;
        item.intrinsic_size = node->intrinsic_size;
        item.image_ready = node->image_ready;
        item.bounds = node->bounds;
        item.shaped = node->shaped;
        item.revision = node->revision;
        for (const auto& child : node->children) item.children.push_back(child->id);
        snapshot.nodes.push_back(std::move(item));
    }
    if (Has(dirty_, Dirty::Layout)) {
        LayoutEngine::Compute(snapshot, viewport_, shaper_);
        for (const auto& item : snapshot.nodes) {
            Node* node = nodes_[item.id.index];
            node->bounds = item.bounds;
            node->shaped = item.shaped;
        }
    }
    auto next = RenderTreeBuilder::Build(snapshot, render_tree_.get());
    auto list = DisplayListBuilder::Build(next, window, font_, generation_ + 1);
    render_tree_ = std::make_unique<RenderTree>(std::move(next));
    ++generation_;
    dirty_ = Dirty::None;
    return list;
}

std::optional<std::string> Scene::Hit(const Node& node, contracts::LogicalPoint point) const {
    if (!Inside(node.bounds, point)) return std::nullopt;
    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        if (auto action = Hit(**it, point)) return action;
    }
    if (!node.action.empty()) return node.action;
    return std::nullopt;
}
std::optional<std::string> Scene::ActionAt(contracts::LogicalPoint point) const {
    return Hit(*root_, point);
}

} // namespace prism::runtime
