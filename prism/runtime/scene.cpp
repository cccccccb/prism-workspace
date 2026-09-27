#include "prism/runtime/scene.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/scene_snapshot.hpp"
#include "prism/runtime/layout_engine.hpp"
#include "prism/runtime/render_tree.hpp"
#include "prism/runtime/display_list_builder.hpp"
#include "prism/runtime/theme_tokens.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <set>

namespace prism::runtime {

struct Scene::Node {
    contracts::NodeId id{};
    Kind kind{Kind::Box};
    Node* parent{};
    Style style{};
    std::map<DslProperty, PropertyValue> properties;
    std::set<DslProperty> explicit_properties;
    std::vector<ThemeRef> theme_refs;
    std::uint64_t allowed_properties{UINT64_MAX};
    std::string text;
    std::string action;
    std::string icon;
    double value{0};
    bool checked{false};
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
        case StoredValueType::String: {
            const auto* text = std::get_if<std::string>(&value);
            if (!text) return false;
            if (id == DslProperty::Align) return *text == "start" || *text == "center" || *text == "end" || *text == "stretch";
            if (id == DslProperty::Justify) return *text == "start" || *text == "center" || *text == "end" || *text == "spaceBetween";
            if (id == DslProperty::Anchor) return *text == "fill" || *text == "left" || *text == "center" || *text == "right";
            if (id == DslProperty::Overflow) return *text == "visible" || *text == "clip";
            if (id == DslProperty::InputShape) return *text == "visible" || *text == "bounds";
            if (id == DslProperty::ImageFit) return *text == "fill" || *text == "contain" || *text == "cover";
            if (id == DslProperty::Icon) {
                constexpr std::string_view icons[] = {"grid", "music", "settings", "folder", "terminal", "play", "pause",
                    "previous", "next", "volume", "wifi", "battery", "search", "sun", "moon", "power", "check", "chevron", "refresh", "cpu", "memory", "heart", "layers", "rectangle", "drop", "wifi-off", "error"};
                return std::find(std::begin(icons), std::end(icons), *text) != std::end(icons);
            }
            return true;
        }
        case StoredValueType::Resource: return std::holds_alternative<contracts::ResourceId>(value) &&
            static_cast<bool>(std::get<contracts::ResourceId>(value));
    }
    return false;
}
} // namespace

Scene::~Scene() = default;

void Scene::Invalidate(Dirty affected) {
    dirty_ = dirty_ | affected;
    if (Has(affected, Dirty::Layout) || Has(affected, Dirty::Paint)) ++pixels_revision_;
}

void Scene::AcknowledgeComposite() {
    dirty_ = static_cast<Dirty>(static_cast<unsigned>(dirty_) & ~static_cast<unsigned>(Dirty::Composite));
}

Scene::Scene(Blueprint root, ShapeText shaper, contracts::ResourceId font,
             std::optional<contracts::ThemeSnapshot> theme)
    : shaper_(std::move(shaper)), font_(font), theme_(std::move(theme)) {
    if (!shaper_) throw std::invalid_argument("Scene requires a text shaper");
    if (theme_) contracts::ValidateTheme(*theme_);
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
    if (theme_) node->style.inner_shadow_y = theme_->controls.inner_shadow_y;
    for (const auto& property : blueprint.properties) if (property.id == DslProperty::Material) {
        if (!theme_) throw std::invalid_argument("Material requires a theme snapshot");
        const auto* name = std::get_if<std::string>(&property.value);
        const auto* material = name ? contracts::FindThemeMaterial(*theme_, *name) : nullptr;
        if (!material) throw std::invalid_argument("Unknown theme material");
        const PropertyAssignment values[] = {
            {DslProperty::Background, material->tint}, {DslProperty::Radius, material->radius},
            {DslProperty::BackdropBlur, material->backdrop_blur},
            {DslProperty::BorderWidth, material->border_width}, {DslProperty::BorderColor, material->border},
            {DslProperty::ShadowBlur, material->shadow_blur}, {DslProperty::ShadowY, material->shadow_y},
            {DslProperty::ShadowColor, material->shadow}, {DslProperty::InnerShadowBlur, material->inner_shadow_blur},
            {DslProperty::InnerShadowY, material->inner_shadow_y}, {DslProperty::InnerShadowColor, material->inner_shadow},
            {DslProperty::InputShape, std::string(material->input_shape == contracts::ThemeInputShape::Bounds ? "bounds" : "visible")}};
        for (const auto& value : values) {
            if (!(node->allowed_properties & PropertyBit(value.id)) || !ValidPropertyValue(value.id, value.value))
                throw std::invalid_argument("Material is not supported by this component");
            node->properties[value.id] = value.value;
            ApplyCachedProperty(*node, value.id, value.value);
        }
    }
    for (auto& property : blueprint.properties) {
        if (!ValidPropertyValue(property.id, property.value) ||
            !(node->allowed_properties & PropertyBit(property.id)))
            throw std::invalid_argument("Invalid Blueprint property");
        node->properties[property.id] = property.value;
        node->explicit_properties.insert(property.id);
        ApplyCachedProperty(*node, property.id, property.value);
    }
    for (const auto& ref : blueprint.theme_refs) {
        if (!theme_) throw std::invalid_argument("Theme reference requires a theme snapshot: " + ref.name);
        const auto value = ResolveThemeToken(*theme_, ref.name);
        if (!value || !ValidPropertyValue(ref.target, *value) ||
            !(node->allowed_properties & PropertyBit(ref.target)))
            throw std::invalid_argument("Invalid theme token for property: " + ref.name);
        node->properties[ref.target] = *value;
        node->explicit_properties.insert(ref.target);
        ApplyCachedProperty(*node, ref.target, *value);
        node->theme_refs.push_back(ref);
    }
    for (auto& binding : blueprint.bindings) {
        if (binding.name.empty() || !(node->allowed_properties & PropertyBit(binding.target)))
            throw std::invalid_argument("Invalid Blueprint binding");
        bindings_[binding.name].push_back({raw, binding.target});
        node->explicit_properties.insert(binding.target);
    }
    for (auto& child : blueprint.children) {
        auto created=MakeNode(std::move(child));
        created->parent=raw;
        node->children.push_back(std::move(created));
    }
    return node;
}

Blueprint Scene::CurrentBlueprint(const Node& node) const {
    Blueprint result;
    result.kind = node.kind;
    result.allowed_properties = node.allowed_properties;
    result.theme_refs = node.theme_refs;
    for (const auto id : node.explicit_properties) {
        if (std::any_of(node.theme_refs.begin(), node.theme_refs.end(),
                        [id](const ThemeRef& ref) { return ref.target == id; })) continue;
        result.properties.push_back({id, CurrentProperty(node, id)});
    }
    for (const auto& child : node.children) result.children.push_back(CurrentBlueprint(*child));
    return result;
}

bool Scene::ApplyTheme(const contracts::ThemeSnapshot& theme, std::string* diagnostic) {
    try {
        contracts::ValidateTheme(theme);
        if (theme_ && *theme_ == theme) {
            if (diagnostic) diagnostic->clear();
            return true;
        }
        Scene candidate(CurrentBlueprint(*root_), shaper_, font_, theme);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            candidate.nodes_[i]->intrinsic_size = nodes_[i]->intrinsic_size;
            candidate.nodes_[i]->image_ready = nodes_[i]->image_ready;
        }
        if (ValidSize(viewport_)) {
            candidate.SetViewport(viewport_);
            (void)candidate.Build({1});
            (void)candidate.SurfaceEffects();
            (void)candidate.InputRegions();
        } else {
            const auto count = std::count_if(candidate.nodes_.begin(), candidate.nodes_.end(),
                [&](const Node* node) { return candidate.IsVisible(*node) && node->style.backdrop_blur > 0; });
            if (count > 8) throw std::length_error("Surface effect region limit is 8");
        }
        // All potentially failing work is complete. Swap prepared value objects
        // into the retained nodes; IDs, actions, bindings and resource IDs stay.
        Dirty affected = Dirty::None;
        const bool controls_changed = !theme_ || theme_->controls != theme.controls;
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (!IsVisible(*nodes_[i])) continue;
            for (unsigned id = 0; id <= static_cast<unsigned>(DslProperty::Visible); ++id) {
                const auto property = static_cast<DslProperty>(id);
                // A material's label does not draw pixels; its resolved values do.
                if (property == DslProperty::Material) continue;
                if (CurrentProperty(*nodes_[i], property) != candidate.CurrentProperty(*candidate.nodes_[i], property))
                    affected = affected | FindProperty(property)->affects;
            }
        }
        if (controls_changed) affected = affected | Dirty::Paint;
        theme_.swap(candidate.theme_);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            std::swap(nodes_[i]->style, candidate.nodes_[i]->style);
            nodes_[i]->properties.swap(candidate.nodes_[i]->properties);
            ++nodes_[i]->revision;
        }
        Invalidate(affected);
        input_dirty_ = true;
        if (diagnostic) diagnostic->clear();
        return true;
    } catch (const std::exception& error) {
        if (diagnostic) *diagnostic = error.what();
        return false;
    }
}

void Scene::ApplyCachedProperty(Node& node, DslProperty id, const PropertyValue& value) {
    switch (id) {
        case DslProperty::Visible: node.style.visible = std::get<bool>(value); break;
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
        case DslProperty::Align: node.style.align = std::get<std::string>(value); break;
        case DslProperty::Justify: node.style.justify = std::get<std::string>(value); break;
        case DslProperty::Anchor: node.style.anchor = std::get<std::string>(value); break;
        case DslProperty::Overflow: node.style.overflow = std::get<std::string>(value); break;
        case DslProperty::Flex: node.style.flex = std::get<double>(value); break;
        case DslProperty::Inset: node.style.inset = std::get<double>(value); break;
        case DslProperty::PaddingX: node.style.padding_x = std::get<double>(value); break;
        case DslProperty::PaddingY: node.style.padding_y = std::get<double>(value); break;
        case DslProperty::BorderWidth: node.style.border_width = std::get<double>(value); break;
        case DslProperty::BorderColor: node.style.border_color = std::get<contracts::Color>(value); break;
        case DslProperty::ShadowBlur: node.style.shadow_blur = std::get<double>(value); break;
        case DslProperty::ShadowY: node.style.shadow_y = std::get<double>(value); break;
        case DslProperty::ShadowColor: node.style.shadow_color = std::get<contracts::Color>(value); break;
        case DslProperty::InnerShadowBlur: node.style.inner_shadow_blur = std::get<double>(value); break;
        case DslProperty::InnerShadowY: node.style.inner_shadow_y = std::get<double>(value); break;
        case DslProperty::InnerShadowColor: node.style.inner_shadow_color = std::get<contracts::Color>(value); break;
        case DslProperty::BackdropBlur: node.style.backdrop_blur = std::get<double>(value); break;
        case DslProperty::Material: node.style.material = std::get<std::string>(value); break;
        case DslProperty::InputShape: node.style.input_shape = std::get<std::string>(value); break;
        case DslProperty::Icon: node.icon = std::get<std::string>(value); break;
        case DslProperty::Value: node.value = std::get<double>(value); break;
        case DslProperty::Checked: node.checked = std::get<bool>(value); break;
        case DslProperty::ImageFit: {
            const auto& name = std::get<std::string>(value);
            node.style.image_fit = name == "contain" ? contracts::ImageFit::Contain :
                name == "cover" ? contracts::ImageFit::Cover : contracts::ImageFit::Fill;
            break;
        }
        case DslProperty::Source:
            node.image = std::get<contracts::ResourceId>(value);
            node.image_ready = false;
            node.intrinsic_size = {};
            break;
    }
}

PropertyValue Scene::CurrentProperty(const Node& node, DslProperty id) const {
    switch (id) {
        case DslProperty::Visible: return node.style.visible;
        case DslProperty::Width: return node.style.width;
        case DslProperty::Height: return node.style.height;
        case DslProperty::Font: return node.style.font_size;
        case DslProperty::Spacing: return node.style.spacing;
        case DslProperty::Padding: return node.style.padding;
        case DslProperty::Material: return node.style.material;
        case DslProperty::InputShape: return node.style.input_shape;
        case DslProperty::InnerShadowY: return node.style.inner_shadow_y;
        case DslProperty::Radius: return node.style.radius;
        case DslProperty::Background: return node.style.background;
        case DslProperty::Foreground: return node.style.foreground;
        case DslProperty::Clip: return node.style.clip;
        case DslProperty::Text: return node.text;
        case DslProperty::Action: return node.action;
        case DslProperty::Source: return node.image;
        case DslProperty::Align: return node.style.align;
        case DslProperty::Justify: return node.style.justify;
        case DslProperty::Anchor: return node.style.anchor;
        case DslProperty::Overflow: return node.style.overflow;
        case DslProperty::Flex: return node.style.flex;
        case DslProperty::Inset: return node.style.inset;
        case DslProperty::PaddingX: return node.style.padding_x;
        case DslProperty::PaddingY: return node.style.padding_y;
        case DslProperty::BorderWidth: return node.style.border_width;
        case DslProperty::BorderColor: return node.style.border_color;
        case DslProperty::ShadowBlur: return node.style.shadow_blur;
        case DslProperty::ShadowY: return node.style.shadow_y;
        case DslProperty::ShadowColor: return node.style.shadow_color;
        case DslProperty::InnerShadowBlur: return node.style.inner_shadow_blur;
        case DslProperty::InnerShadowColor: return node.style.inner_shadow_color;
        case DslProperty::BackdropBlur: return node.style.backdrop_blur;
        case DslProperty::Icon: return node.icon;
        case DslProperty::Value: return node.value;
        case DslProperty::Checked: return node.checked;
        case DslProperty::ImageFit: return std::string(node.style.image_fit==contracts::ImageFit::Cover ? "cover" :
            node.style.image_fit==contracts::ImageFit::Contain ? "contain" : "fill");
        default: {
            const auto found = node.properties.find(id);
            if (found != node.properties.end()) return found->second;
            const auto* spec = FindProperty(id);
            if (spec->stored_type == StoredValueType::String) return std::string{};
            if (spec->stored_type == StoredValueType::Color) return contracts::Color{0,0,0,0};
            if (spec->stored_type == StoredValueType::Boolean) return false;
            return 0.0;
        }
    }
    return {};
}

Scene::Node* Scene::Find(contracts::NodeId id) const {
    if (!id || id.index >= nodes_.size()) return nullptr;
    Node* node = nodes_[id.index];
    return node->id == id ? node : nullptr;
}
bool Scene::IsVisible(const Node& node) const {
    for (const Node* current=&node;current;current=current->parent)
        if (!current->style.visible) return false;
    return true;
}

contracts::NodeId Scene::RootId() const { return root_->id; }
contracts::LogicalRect Scene::Bounds(contracts::NodeId id) const {
    Node* node = Find(id);
    return node ? node->bounds : contracts::LogicalRect{};
}
bool Scene::IsVisible(contracts::NodeId id) const {
    const auto* node=Find(id);
    return node && IsVisible(*node);
}

bool Scene::SetSlot(std::string_view name, std::string value) {
    return SetBinding(name, std::move(value));
}

bool Scene::AcceptsBinding(std::string_view name, const PropertyValue& value) const {
    auto it = bindings_.find(std::string(name));
    if (it == bindings_.end()) return false;
    for (const auto& target : it->second)
        if (!ValidPropertyValue(target.property, value)) return false;
    return true;
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
    if (!node || property == DslProperty::Material || !(node->allowed_properties & PropertyBit(property)) ||
        !ValidPropertyValue(property, value)) return false;
    const auto previous=CurrentProperty(*node,property);
    if (previous == value) return false;
    const bool was_visible=IsVisible(*node);
    node->properties[property] = value;
    node->explicit_properties.insert(property);
    std::erase_if(node->theme_refs, [property](const ThemeRef& ref) { return ref.target == property; });
    ApplyCachedProperty(*node, property, value);
    ++node->revision;
    // Retain all state while concealed, including cache revisions and resolved
    // style overrides. Revealing any ancestor triggers a full layout/snapshot.
    // A child becoming locally visible under a hidden parent stays concealed.
    if (!was_visible && !IsVisible(*node)) return true;
    if (property == DslProperty::Visible && !node->style.visible) {
        if (hovered_ && !IsVisible(*hovered_)) { ++hovered_->revision; hovered_=nullptr; }
        if (focused_ && !IsVisible(*focused_)) { ++focused_->revision; focused_=nullptr; }
    }
    const Dirty affected = FindProperty(property)->affects;
    const bool input_shape=Has(affected,Dirty::Layout) || property==DslProperty::Radius ||
        property==DslProperty::Clip || property==DslProperty::Overflow || property==DslProperty::InputShape ||
        (property==DslProperty::Background && bool(std::get<contracts::Color>(previous).a)!=bool(std::get<contracts::Color>(value).a)) ||
        (property==DslProperty::BackdropBlur && (std::get<double>(previous)>0)!=(std::get<double>(value)>0)) ||
        (property==DslProperty::Action && std::get<std::string>(previous).empty()!=std::get<std::string>(value).empty());
    if(input_shape)input_dirty_=true;
    Invalidate(affected | (input_shape && affected == Dirty::None ? Dirty::Composite : Dirty::None));
    return true;
}

bool Scene::SetViewport(contracts::LogicalSize size) {
    if (!ValidSize(size)) return false;
    if (viewport_.width != size.width || viewport_.height != size.height) {
        viewport_ = size;
        input_dirty_=true;
        Invalidate(Dirty::Layout | Dirty::Paint);
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
            input_dirty_=true;
            ++node->revision;
            if (IsVisible(*node)) Invalidate(Dirty::Paint | (affects_layout ? Dirty::Layout : Dirty::None));
            changed = true;
        }
    }
    return changed;
}

std::optional<contracts::DisplayList> Scene::Build(contracts::WindowId window) {
    ++build_calls_;
    if (!window || !ValidSize(viewport_) ||
        (!Has(dirty_, Dirty::Layout) && !Has(dirty_, Dirty::Paint))) return std::nullopt;
    SceneSnapshot snapshot;
    if (theme_) snapshot.controls = theme_->controls;
    snapshot.root = root_->id;
    snapshot.nodes.reserve(nodes_.size());
    for (const Node* node : nodes_) {
        SnapshotNode item;
        item.id = node->id;
        item.kind = node->kind;
        item.style = node->style;
        item.text = node->text;
        item.icon = node->icon;
        item.value = node->value;
        item.checked = node->checked;
        item.hovered = node == hovered_;
        item.focused = node == focused_;
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
        ++layout_count_;
        input_dirty_=true;
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
    dirty_ = Has(dirty_, Dirty::Composite) ? Dirty::Composite : Dirty::None;
    return list;
}

std::optional<std::string> Scene::Hit(const Node& node, contracts::LogicalPoint point) const {
    if (!node.style.visible) return std::nullopt;
    const bool inside=Inside(node.bounds,point);
    if (!inside && (node.style.clip || node.style.overflow=="clip")) return std::nullopt;
    const double radius = std::min({node.style.radius, node.bounds.width / 2, node.bounds.height / 2});
    if (radius > 0 && (node.style.clip || node.style.overflow == "clip" || !node.action.empty())) {
        const double cx = std::clamp(point.x, node.bounds.x + radius, node.bounds.x + node.bounds.width - radius);
        const double cy = std::clamp(point.y, node.bounds.y + radius, node.bounds.y + node.bounds.height - radius);
        if ((point.x-cx)*(point.x-cx)+(point.y-cy)*(point.y-cy) > radius*radius) return std::nullopt;
    }
    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        if (auto action = Hit(**it, point)) return action;
    }
    if (inside && !node.action.empty()) return node.action;
    return std::nullopt;
}
std::vector<contracts::SurfaceEffectRegion> Scene::SurfaceEffects() const {
    std::vector<contracts::SurfaceEffectRegion> result;
    using Shape=contracts::SurfaceInputRegion;
    using Rect=contracts::LogicalRect;
    const auto normalized=[](Shape shape) {
        shape.corner_radius=std::clamp(shape.corner_radius,0.0,
            std::min(shape.bounds.width,shape.bounds.height)/2);
        return shape;
    };
    const auto same=[](Rect a,Rect b) {
        return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
    };
    const auto inside=[](contracts::LogicalPoint point,Shape shape) {
        const auto& b=shape.bounds;
        if(point.x<b.x || point.y<b.y || point.x>b.x+b.width || point.y>b.y+b.height)return false;
        const auto r=shape.corner_radius;
        const auto x=std::clamp(point.x,b.x+r,b.x+b.width-r);
        const auto y=std::clamp(point.y,b.y+r,b.y+b.height-r);
        return (point.x-x)*(point.x-x)+(point.y-y)*(point.y-y)<=r*r+1e-7;
    };
    const auto contains=[&](Shape outer,Rect box) {
        return inside({box.x,box.y},outer) && inside({box.x+box.width,box.y},outer) &&
            inside({box.x,box.y+box.height},outer) && inside({box.x+box.width,box.y+box.height},outer);
    };
    const auto intersect=[&](Shape a,Shape b)->std::optional<Shape> {
        const double x=std::max(a.bounds.x,b.bounds.x),y=std::max(a.bounds.y,b.bounds.y);
        const Rect box{x,y,std::max(0.0,std::min(a.bounds.x+a.bounds.width,b.bounds.x+b.bounds.width)-x),
            std::max(0.0,std::min(a.bounds.y+a.bounds.height,b.bounds.y+b.bounds.height)-y)};
        if(box.width==0 || box.height==0)return std::nullopt;
        if(same(a.bounds,b.bounds))return Shape{box,std::max(a.corner_radius,b.corner_radius)};
        if(same(box,a.bounds) && contains(b,a.bounds))return a;
        if(same(box,b.bounds) && contains(a,b.bounds))return b;
        if(contains(a,box) && contains(b,box))return Shape{box,0};
        // The v1 protocol has one uniform-radius rounded rectangle. Cropping
        // its curved corners, or combining offset curved clips, can produce
        // asymmetric masks that cannot be transmitted faithfully.
        throw std::runtime_error("Unsupported backdrop clipping: intersection is not a v1 rounded rectangle");
    };
    std::vector<Shape> clips{{{0,0,viewport_.width,viewport_.height},0}};
    const auto collect=[&](const auto& self,const Node& node)->void {
        if(!node.style.visible || node.bounds.width<=0 || node.bounds.height<=0)return;
        const bool clipped=node.style.clip || node.style.overflow=="clip";
        if(clipped)clips.push_back(normalized({node.bounds,node.style.radius}));
        if(node.style.backdrop_blur>0) {
            std::optional<Shape> shape=normalized({node.bounds,node.style.radius});
            for(const auto& clip:clips) {
                shape=intersect(*shape,clip);
                if(!shape)break;
            }
            if(shape) {
                const auto& b=shape->bounds;
                if(std::abs(b.x)>8192 || std::abs(b.y)>8192 || b.width>8192 || b.height>8192)
                    throw std::runtime_error("Unsupported backdrop bounds: v1 maximum is 8192 logical pixels");
                result.push_back({b,shape->corner_radius,node.style.backdrop_blur});
            }
        }
        for(const auto& child:node.children)self(self,*child);
        if(clipped)clips.pop_back();
    };
    collect(collect,*root_);
    if(result.size()>8) throw std::length_error("Surface effect region limit is 8");
    return result;
}
const std::vector<contracts::SurfaceInputRegion>& Scene::InputRegions() const {
    if(!input_dirty_)return input_regions_;
    input_regions_.clear();
    using Shape=contracts::SurfaceInputRegion;
    using Rect=contracts::LogicalRect;
    const auto same=[](Rect a,Rect b) {return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;};
    const auto intersect=[](Rect a,Rect b) {
        const auto x=std::max(a.x,b.x),y=std::max(a.y,b.y);
        return Rect{x,y,std::max(0.0,std::min(a.x+a.width,b.x+b.width)-x),
            std::max(0.0,std::min(a.y+a.height,b.y+b.height)-y)};
    };
    std::vector<Shape> clips;
    const auto add=[&](Shape shape) {
        auto bounds=shape.bounds;
        double radius=shape.corner_radius;
        bool identical=true,rectangular=radius==0;
        for(const auto& clip:clips) {
            bounds=intersect(bounds,clip.bounds);
            identical=identical && same(shape.bounds,clip.bounds);
            rectangular=rectangular && clip.corner_radius==0;
            radius=std::max(radius,clip.corner_radius);
        }
        if(bounds.width<=0 || bounds.height<=0)return;
        if(clips.empty() || identical || rectangular) {
            input_regions_.push_back({bounds,rectangular?0:radius});
            return;
        }
        // Arbitrary rounded intersections are not themselves rounded rects.
        // Resolve the exact logical-pixel input mask once, using the same
        // pixel-center convention as the Wayland region rasterizer.
        const auto span=[](Shape region,double y) {
            const auto& b=region.bounds;
            const double r=std::clamp(region.corner_radius,0.0,std::min(b.width,b.height)/2);
            const double edge=std::min(y-b.y,b.y+b.height-y);
            double inset=0;
            if(edge<r)inset=r-std::sqrt(std::max(0.0,r*r-(r-edge)*(r-edge)));
            return std::pair{b.x+inset,b.x+b.width-inset};
        };
        for(int y=static_cast<int>(std::ceil(bounds.y));y<static_cast<int>(std::floor(bounds.y+bounds.height));++y) {
            auto [left,right]=span(shape,y+.5);
            for(const auto& clip:clips) {
                const auto [a,b]=span(clip,y+.5);
                left=std::max(left,a);right=std::min(right,b);
            }
            const double first=std::ceil(left),last=std::floor(right);
            if(last>first)input_regions_.push_back({{first,double(y),last-first,1},0});
        }
    };
    const auto collect=[&](const auto& self,const Node& node)->void {
        if(!node.style.visible || node.bounds.width<=0 || node.bounds.height<=0)return;
        const bool clipped=node.style.clip || node.style.overflow=="clip";
        if(clipped)clips.push_back({node.bounds,node.style.radius});
        const bool material=node.style.input_shape=="bounds" || node.style.background.a || node.style.backdrop_blur>0 ||
            !node.action.empty() || node.kind==Kind::Image;
        if(material)add({node.bounds,node.style.radius});
        // A material clip already covers all visible descendants. A visible
        // overflow child can extend the union beyond its parent's region.
        if(!(material && clipped))for(const auto& child:node.children)self(self,*child);
        if(clipped)clips.pop_back();
    };
    collect(collect,*root_);
    input_dirty_=false;
    return input_regions_;
}
bool Scene::SetPointer(contracts::LogicalPoint point) {
    Node* next=nullptr;
    const auto action=ActionAt(point);
    if(action) for(auto it=nodes_.rbegin();it!=nodes_.rend();++it)
        if(IsVisible(**it) && (*it)->action==*action && Inside((*it)->bounds,point)) {next=*it;break;}
    if(next==hovered_)return false;
    if(hovered_)++hovered_->revision;
    hovered_=next;
    if(hovered_)++hovered_->revision;
    Invalidate(Dirty::Paint);
    return true;
}
bool Scene::FocusNext() {
    std::vector<Node*> actions;
    for(auto* node:nodes_)if(IsVisible(*node) && !node->action.empty())actions.push_back(node);
    if(actions.empty())return false;
    const auto it=std::find(actions.begin(),actions.end(),focused_);
    Node* next=it==actions.end() || std::next(it)==actions.end() ? actions.front() : *std::next(it);
    if(focused_)++focused_->revision;
    focused_=next;++focused_->revision;
    Invalidate(Dirty::Paint);
    return true;
}
std::optional<std::string> Scene::FocusedAction() const {
    return focused_ && IsVisible(*focused_) ? std::optional<std::string>(focused_->action) : std::nullopt;
}
std::optional<std::string> Scene::ActionAt(contracts::LogicalPoint point) const {
    return Hit(*root_, point);
}

} // namespace prism::runtime
