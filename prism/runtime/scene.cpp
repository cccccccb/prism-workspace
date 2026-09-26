#include "prism/runtime/scene.hpp"
#include "prism/runtime/dsl_schema.hpp"
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
    dirty_ = dirty_ | FindProperty(property)->affects;
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
            dirty_ = dirty_ | Dirty::Paint | (affects_layout ? Dirty::Layout : Dirty::None);
            changed = true;
        }
    }
    return changed;
}

void Scene::Layout(Node& node, contracts::LogicalRect bounds) {
    node.bounds = bounds;
    if (node.kind == Kind::Text) {
        node.shaped = shaper_(node.text, node.style.font_size);
        return;
    }
    if (node.kind == Kind::Image) return;
    const double pad = std::max(0.0, node.style.padding);
    const double x = bounds.x + pad, y = bounds.y + pad;
    const double width = std::max(0.0, bounds.width - 2 * pad);
    const double height = std::max(0.0, bounds.height - 2 * pad);
    if (node.kind == Kind::Box) {
        for (auto& child : node.children) {
            auto& s = child->style;
            const double w = s.width > 0 ? std::min(s.width, width) : width;
            const double h = s.height > 0 ? std::min(s.height, height)
                : child->kind == Kind::Text ? std::min(s.font_size * 1.4, height)
                : child->kind == Kind::Image && child->image_ready ? std::min(child->intrinsic_size.height, height)
                : height;
            const double image_width = child->kind == Kind::Image && child->image_ready && s.width <= 0
                ? std::min(child->intrinsic_size.width, width) : w;
            Layout(*child, {x, y, image_width, h});
        }
        return;
    }
    const bool row = node.kind == Kind::Row;
    const double main = row ? width : height;
    const double cross = row ? height : width;
    const double gap = std::max(0.0, node.style.spacing);
    const double total_gap = gap * (node.children.empty() ? 0 : node.children.size() - 1);
    double fixed = 0;
    std::size_t flexible = 0;
    for (const auto& child : node.children) {
        const double explicit_size = row ? child->style.width : child->style.height;
        if (explicit_size > 0) fixed += explicit_size;
        else if (!row && child->kind == Kind::Text) fixed += child->style.font_size * 1.4;
        else if (child->kind == Kind::Image && child->image_ready)
            fixed += row ? child->intrinsic_size.width : child->intrinsic_size.height;
        else ++flexible;
    }
    const double remaining = std::max(0.0, main - total_gap - fixed);
    const double flex_size = flexible ? remaining / flexible : 0;
    double cursor = row ? x : y;
    for (auto& child : node.children) {
        const double explicit_size = row ? child->style.width : child->style.height;
        const double intrinsic = !row && child->kind == Kind::Text ? child->style.font_size * 1.4
            : child->kind == Kind::Image && child->image_ready
                ? (row ? child->intrinsic_size.width : child->intrinsic_size.height) : flex_size;
        const double length = std::max(0.0, std::min(explicit_size > 0 ? explicit_size : intrinsic,
            std::max(0.0, (row ? x + width : y + height) - cursor)));
        const double cross_explicit = row ? child->style.height : child->style.width;
        const double cross_intrinsic = child->kind == Kind::Image && child->image_ready
            ? (row ? child->intrinsic_size.height : child->intrinsic_size.width) : cross;
        const double other = std::min(cross_explicit > 0 ? cross_explicit : cross_intrinsic, cross);
        Layout(*child, row ? contracts::LogicalRect{cursor, y, length, other}
                           : contracts::LogicalRect{x, cursor, other, length});
        cursor += length + gap;
    }
}

void Scene::Paint(const Node& node, contracts::DisplayList& list) const {
    if (node.bounds.width <= 0 || node.bounds.height <= 0) return;
    if (node.style.clip) list.commands.emplace_back(contracts::PushClipRect{node.bounds});
    if (node.style.background.a) {
        if (node.style.radius > 0)
            list.commands.emplace_back(contracts::FillRoundedRect{node.bounds, node.style.radius, node.style.background});
        else list.commands.emplace_back(contracts::FillRect{node.bounds, node.style.background});
    }
    if (node.kind == Kind::Text && !node.shaped.glyphs.empty()) {
        contracts::DrawGlyphRun run;
        run.font = font_;
        run.color = node.style.foreground;
        run.font_size = node.style.font_size;
        for (auto glyph : node.shaped.glyphs) {
            glyph.origin.x += node.bounds.x;
            glyph.origin.y += node.bounds.y;
            run.glyphs.push_back(glyph);
        }
        list.commands.emplace_back(std::move(run));
    }
    if (node.kind == Kind::Image && node.image_ready)
        list.commands.emplace_back(contracts::DrawImage{node.image, node.bounds});
    for (const auto& child : node.children) Paint(*child, list);
    if (node.style.clip) list.commands.emplace_back(contracts::PopClip{});
}

std::optional<contracts::DisplayList> Scene::Build(contracts::WindowId window) {
    if (!window || !ValidSize(viewport_) || dirty_ == Dirty::None) return std::nullopt;
    if (Has(dirty_, Dirty::Layout)) Layout(*root_, {0, 0, viewport_.width, viewport_.height});
    contracts::DisplayList list;
    list.window = window;
    list.generation = ++generation_;
    Paint(*root_, list);
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
