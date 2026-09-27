#include "prism/runtime/display_list_builder.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/layout_engine.hpp"
#include "prism/runtime/render_tree.hpp"
#include "prism/runtime/scene_snapshot.hpp"
#include "prism/runtime/theme_tokens.hpp"
#include "scene_p.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace prism::runtime {

Scene::~Scene() = default;

void Scene::Invalidate(Dirty affected)
{

    dirty_ = dirty_ | affected;
    if (Has(affected, Dirty::Layout) || Has(affected, Dirty::Paint)) {
        ++pixels_revision_;
    }
}

void Scene::AcknowledgeComposite()
{

    dirty_ = static_cast<Dirty>(static_cast<unsigned>(dirty_) &
                                ~static_cast<unsigned>(Dirty::Composite));
}

Scene::Scene(Blueprint root, ShapeText shaper, contracts::ResourceId font,
             std::optional<contracts::ThemeSnapshot> theme)
    : shaper_(std::move(shaper)), font_(font), theme_(std::move(theme))
{
    if (!shaper_) {
        throw std::invalid_argument("Scene requires a text shaper");
    }
    if (theme_) {
        contracts::ValidateTheme(*theme_);
    }
    root_ = MakeNode(std::move(root));
}

std::unique_ptr<Scene::Node> Scene::MakeNode(Blueprint blueprint)
{
    auto node = std::make_unique<Node>();
    if (nodes_.size() >= UINT32_MAX) {
        throw std::length_error("Scene node limit");
    }
    node->id = {static_cast<std::uint32_t>(nodes_.size()), 1};
    Node *raw = node.get();
    nodes_.push_back(raw);
    node->kind = blueprint.kind;
    node->allowed_properties = blueprint.allowed_properties;
    if (theme_) {
        node->style.inner_shadow_y = theme_->controls.inner_shadow_y;
    }
    for (const auto &property : blueprint.properties) {
        if (property.id == DslProperty::Material) {
            if (!theme_) {
                throw std::invalid_argument("Material requires a theme snapshot");
            }
            const auto *name = std::get_if<std::string>(&property.value);
            const auto *material = name ? contracts::FindThemeMaterial(*theme_, *name) : nullptr;
            if (!material) {
                throw std::invalid_argument("Unknown theme material");
            }
            const PropertyAssignment values[] = {
                {DslProperty::Background, material->tint},
                {DslProperty::Radius, material->radius},
                {DslProperty::BackdropBlur, material->backdrop_blur},
                {DslProperty::BorderWidth, material->border_width},
                {DslProperty::BorderColor, material->border},
                {DslProperty::ShadowBlur, material->shadow_blur},
                {DslProperty::ShadowY, material->shadow_y},
                {DslProperty::ShadowColor, material->shadow},
                {DslProperty::InnerShadowBlur, material->inner_shadow_blur},
                {DslProperty::InnerShadowY, material->inner_shadow_y},
                {DslProperty::InnerShadowColor, material->inner_shadow},
                {DslProperty::InputShape,
                 std::string(material->input_shape == contracts::ThemeInputShape::Bounds
                                 ? "bounds"
                                 : "visible")}};
            for (const auto &value : values) {
                if (!(node->allowed_properties & PropertyBit(value.id)) ||
                    !scene_detail::ValidPropertyValue(value.id, value.value)) {
                    throw std::invalid_argument("Material is not supported by this component");
                }
                node->properties[value.id] = value.value;
                ApplyCachedProperty(*node, value.id, value.value);
            }
        }
    }
    for (auto &property : blueprint.properties) {
        if (!scene_detail::ValidPropertyValue(property.id, property.value) ||
            !(node->allowed_properties & PropertyBit(property.id))) {
            throw std::invalid_argument("Invalid Blueprint property");
        }
        node->properties[property.id] = property.value;
        node->explicit_properties.insert(property.id);
        ApplyCachedProperty(*node, property.id, property.value);
    }
    for (const auto &ref : blueprint.theme_refs) {
        if (!theme_) {
            throw std::invalid_argument("Theme reference requires a theme snapshot: " + ref.name);
        }
        const auto value = ResolveThemeToken(*theme_, ref.name);
        if (!value || !scene_detail::ValidPropertyValue(ref.target, *value) ||
            !(node->allowed_properties & PropertyBit(ref.target))) {
            throw std::invalid_argument("Invalid theme token for property: " + ref.name);
        }
        node->properties[ref.target] = *value;
        node->explicit_properties.insert(ref.target);
        ApplyCachedProperty(*node, ref.target, *value);
        node->theme_refs.push_back(ref);
    }
    for (auto &binding : blueprint.bindings) {
        if (binding.name.empty() || !(node->allowed_properties & PropertyBit(binding.target))) {
            throw std::invalid_argument("Invalid Blueprint binding");
        }
        bindings_[binding.name].push_back({raw, binding.target});
        node->explicit_properties.insert(binding.target);
    }
    for (auto &child : blueprint.children) {
        auto created = MakeNode(std::move(child));
        created->parent = raw;
        node->children.push_back(std::move(created));
    }
    return node;
}

Blueprint Scene::CurrentBlueprint(const Node &node) const
{
    Blueprint result;
    result.kind = node.kind;
    result.allowed_properties = node.allowed_properties;
    result.theme_refs = node.theme_refs;
    for (const auto id : node.explicit_properties) {
        if (std::any_of(node.theme_refs.begin(), node.theme_refs.end(),
                        [id](const ThemeRef &ref) { return ref.target == id; })) {
            continue;
        }
        result.properties.push_back({id, CurrentProperty(node, id)});
    }
    for (const auto &child : node.children) {
        result.children.push_back(CurrentBlueprint(*child));
    }
    return result;
}

bool Scene::ApplyTheme(const contracts::ThemeSnapshot &theme, std::string *diagnostic)
{
    try {
        contracts::ValidateTheme(theme);
        if (theme_ && *theme_ == theme) {
            if (diagnostic) {
                diagnostic->clear();
            }
            return true;
        }
        Scene candidate(CurrentBlueprint(*root_), shaper_, font_, theme);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            candidate.nodes_[i]->intrinsic_size = nodes_[i]->intrinsic_size;
            candidate.nodes_[i]->image_ready = nodes_[i]->image_ready;
        }
        if (scene_detail::ValidSize(viewport_)) {
            candidate.SetViewport(viewport_);
            (void)candidate.Build({1});
            (void)candidate.SurfaceEffects();
            (void)candidate.InputRegions();
        } else {
            const auto count = std::count_if(
                candidate.nodes_.begin(), candidate.nodes_.end(), [&](const Node *node) {
                    return candidate.IsVisible(*node) && node->style.backdrop_blur > 0;
                });
            if (count > 8) {
                throw std::length_error("Surface effect region limit is 8");
            }
        }
        // All potentially failing work is complete. Swap prepared value objects
        // into the retained nodes; IDs, actions, bindings and resource IDs stay.
        Dirty affected = Dirty::None;
        const bool controls_changed = !theme_ || theme_->controls != theme.controls;
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (!IsVisible(*nodes_[i])) {
                continue;
            }
            for (unsigned id = 0; id <= static_cast<unsigned>(DslProperty::Visible); ++id) {
                const auto property = static_cast<DslProperty>(id);
                // A material's label does not draw pixels; its resolved values do.
                if (property == DslProperty::Material) {
                    continue;
                }
                if (CurrentProperty(*nodes_[i], property) !=
                    candidate.CurrentProperty(*candidate.nodes_[i], property)) {
                    affected = affected | FindProperty(property)->affects;
                }
            }
        }
        if (controls_changed) {
            affected = affected | Dirty::Paint;
        }
        theme_.swap(candidate.theme_);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            std::swap(nodes_[i]->style, candidate.nodes_[i]->style);
            nodes_[i]->properties.swap(candidate.nodes_[i]->properties);
            ++nodes_[i]->revision;
        }
        Invalidate(affected);
        input_dirty_ = true;
        if (diagnostic) {
            diagnostic->clear();
        }
        return true;
    } catch (const std::exception &error) {
        if (diagnostic) {
            *diagnostic = error.what();
        }
        return false;
    }
}

Scene::Node *Scene::Find(contracts::NodeId id) const
{
    if (!id || id.index >= nodes_.size()) {
        return nullptr;
    }
    Node *node = nodes_[id.index];
    return node->id == id ? node : nullptr;
}

bool Scene::IsVisible(const Node &node) const
{
    for (const Node *current = &node; current; current = current->parent) {
        if (!current->style.visible) {
            return false;
        }
    }
    return true;
}

contracts::NodeId Scene::RootId() const
{
    return root_->id;
}

contracts::LogicalRect Scene::Bounds(contracts::NodeId id) const
{
    Node *node = Find(id);
    return node ? node->bounds : contracts::LogicalRect{};
}

bool Scene::IsVisible(contracts::NodeId id) const
{
    const auto *node = Find(id);
    return node && IsVisible(*node);
}

bool Scene::SetSlot(std::string_view name, std::string value)
{
    return SetBinding(name, std::move(value));
}

bool Scene::AcceptsBinding(std::string_view name, const PropertyValue &value) const
{
    auto it = bindings_.find(std::string(name));
    if (it == bindings_.end()) {
        return false;
    }
    for (const auto &target : it->second) {
        if (!scene_detail::ValidPropertyValue(target.property, value)) {
            return false;
        }
    }
    return true;
}

bool Scene::SetBinding(std::string_view name, PropertyValue value)
{
    auto it = bindings_.find(std::string(name));
    if (it == bindings_.end()) {
        return false;
    }
    for (const auto &target : it->second) {
        if (!scene_detail::ValidPropertyValue(target.property, value)) {
            return false;
        }
    }
    bool changed = false;
    for (const auto &target : it->second) {
        changed = SetProperty(target.node->id, target.property, value) || changed;
    }
    return changed;
}

bool Scene::SetProperty(contracts::NodeId id, DslProperty property, PropertyValue value)
{
    Node *node = Find(id);
    if (!node || property == DslProperty::Material ||
        !(node->allowed_properties & PropertyBit(property)) ||
        !scene_detail::ValidPropertyValue(property, value)) {
        return false;
    }
    const auto previous = CurrentProperty(*node, property);
    if (previous == value) {
        return false;
    }
    const bool was_visible = IsVisible(*node);
    node->properties[property] = value;
    node->explicit_properties.insert(property);
    std::erase_if(node->theme_refs,
                  [property](const ThemeRef &ref) { return ref.target == property; });
    ApplyCachedProperty(*node, property, value);
    ++node->revision;
    // Retain all state while concealed, including cache revisions and resolved
    // style overrides. Revealing any ancestor triggers a full layout/snapshot.
    // A child becoming locally visible under a hidden parent stays concealed.
    if (!was_visible && !IsVisible(*node)) {
        return true;
    }
    if (property == DslProperty::Visible && !node->style.visible) {
        if (hovered_ && !IsVisible(*hovered_)) {
            ++hovered_->revision;
            hovered_ = nullptr;
        }
        if (focused_ && !IsVisible(*focused_)) {
            ++focused_->revision;
            focused_ = nullptr;
        }
    }
    const Dirty affected = FindProperty(property)->affects;
    const bool input_shape =
        Has(affected, Dirty::Layout) || property == DslProperty::Radius ||
        property == DslProperty::Clip || property == DslProperty::Overflow ||
        property == DslProperty::InputShape ||
        (property == DslProperty::Background && bool(std::get<contracts::Color>(previous).a) !=
                                                    bool(std::get<contracts::Color>(value).a)) ||
        (property == DslProperty::BackdropBlur &&
         (std::get<double>(previous) > 0) != (std::get<double>(value) > 0)) ||
        (property == DslProperty::Action &&
         std::get<std::string>(previous).empty() != std::get<std::string>(value).empty());
    if (input_shape) {
        input_dirty_ = true;
    }
    Invalidate(affected |
               (input_shape && affected == Dirty::None ? Dirty::Composite : Dirty::None));
    return true;
}

bool Scene::SetViewport(contracts::LogicalSize size)
{
    if (!scene_detail::ValidSize(size)) {
        return false;
    }
    if (viewport_.width != size.width || viewport_.height != size.height) {
        viewport_ = size;
        input_dirty_ = true;
        Invalidate(Dirty::Layout | Dirty::Paint);
    }
    return true;
}

bool Scene::SetBackground(contracts::NodeId id, contracts::Color color)
{
    return SetProperty(id, DslProperty::Background, color);
}

bool Scene::ImageReady(contracts::ResourceId image, contracts::LogicalSize intrinsic_size)
{
    if (!image || !scene_detail::ValidSize(intrinsic_size)) {
        return false;
    }
    bool changed = false;
    for (Node *node : nodes_) {
        if (node->kind != Kind::Image || node->image != image) {
            continue;
        }
        if (!node->image_ready || node->intrinsic_size.width != intrinsic_size.width ||
            node->intrinsic_size.height != intrinsic_size.height) {
            const bool affects_layout = node->style.width <= 0 || node->style.height <= 0;
            node->intrinsic_size = intrinsic_size;
            node->image_ready = true;
            input_dirty_ = true;
            ++node->revision;
            if (IsVisible(*node)) {
                Invalidate(Dirty::Paint | (affects_layout ? Dirty::Layout : Dirty::None));
            }
            changed = true;
        }
    }
    return changed;
}

std::optional<contracts::DisplayList> Scene::Build(contracts::WindowId window)
{
    ++build_calls_;
    if (!window || !scene_detail::ValidSize(viewport_) ||
        (!Has(dirty_, Dirty::Layout) && !Has(dirty_, Dirty::Paint))) {
        return std::nullopt;
    }

    SceneSnapshot snapshot;
    if (theme_) {
        snapshot.controls = theme_->controls;
    }
    snapshot.root = root_->id;
    snapshot.nodes.reserve(nodes_.size());
    for (const Node *node : nodes_) {
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
        for (const auto &child : node->children) {
            item.children.push_back(child->id);
        }
        snapshot.nodes.push_back(std::move(item));
    }
    if (Has(dirty_, Dirty::Layout)) {
        LayoutEngine::Compute(snapshot, viewport_, shaper_);
        ++layout_count_;
        input_dirty_ = true;
        for (const auto &item : snapshot.nodes) {
            Node *node = nodes_[item.id.index];
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

std::optional<std::string> Scene::Hit(const Node &node, contracts::LogicalPoint point) const
{
    if (!node.style.visible) {
        return std::nullopt;
    }
    const bool inside = scene_detail::Inside(node.bounds, point);
    if (!inside && (node.style.clip || node.style.overflow == "clip")) {
        return std::nullopt;
    }
    const double radius =
        std::min({node.style.radius, node.bounds.width / 2, node.bounds.height / 2});
    if (radius > 0 && (node.style.clip || node.style.overflow == "clip" || !node.action.empty())) {
        const double cx =
            std::clamp(point.x, node.bounds.x + radius, node.bounds.x + node.bounds.width - radius);
        const double cy = std::clamp(point.y, node.bounds.y + radius,
                                     node.bounds.y + node.bounds.height - radius);
        if ((point.x - cx) * (point.x - cx) + (point.y - cy) * (point.y - cy) > radius * radius) {
            return std::nullopt;
        }
    }
    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        if (auto action = Hit(**it, point)) {
            return action;
        }
    }
    if (inside && !node.action.empty()) {
        return node.action;
    }
    return std::nullopt;
}

bool Scene::SetPointer(contracts::LogicalPoint point)
{
    Node *next = nullptr;
    const auto action = ActionAt(point);
    if (action) {
        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
            if (IsVisible(**it) && (*it)->action == *action &&
                scene_detail::Inside((*it)->bounds, point)) {
                next = *it;
                break;
            }
        }
    }
    if (next == hovered_) {
        return false;
    }
    if (hovered_) {
        ++hovered_->revision;
    }
    hovered_ = next;
    if (hovered_) {
        ++hovered_->revision;
    }
    Invalidate(Dirty::Paint);
    return true;
}

bool Scene::FocusNext()
{
    std::vector<Node *> actions;
    for (auto *node : nodes_) {
        if (IsVisible(*node) && !node->action.empty()) {
            actions.push_back(node);
        }
    }
    if (actions.empty()) {
        return false;
    }
    const auto it = std::find(actions.begin(), actions.end(), focused_);
    Node *next =
        it == actions.end() || std::next(it) == actions.end() ? actions.front() : *std::next(it);
    if (focused_) {
        ++focused_->revision;
    }
    focused_ = next;
    ++focused_->revision;
    Invalidate(Dirty::Paint);
    return true;
}

std::optional<std::string> Scene::FocusedAction() const
{
    return focused_ && IsVisible(*focused_) ? std::optional<std::string>(focused_->action)
                                            : std::nullopt;
}

std::optional<std::string> Scene::ActionAt(contracts::LogicalPoint point) const
{
    return Hit(*root_, point);
}

} // namespace prism::runtime
