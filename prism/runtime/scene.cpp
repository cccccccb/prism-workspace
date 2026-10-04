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
    if (Has(affected, Dirty::Layout)) {
        hit_geometry_dirty_ = true;
        input_snapshot_dirty_ = true;
    }
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
    : Scene(EmptyConstruction{}, std::move(shaper), font, std::move(theme))
{
    root_ = MakeNode(std::move(root));
    PrepareInteractionTree();
}

Scene::Scene(EmptyConstruction, ShapeText shaper, contracts::ResourceId font,
             std::optional<contracts::ThemeSnapshot> theme)
    : shaper_(std::move(shaper)), font_(font), input_state_(std::make_unique<InputState>()),
      theme_(std::move(theme))
{
    if (!shaper_) {
        throw std::invalid_argument("Scene requires a text shaper");
    }
    if (theme_) {
        contracts::ValidateTheme(*theme_);
    }
}

Scene::Node *Scene::Find(contracts::NodeId id) const
{
    if (!id || id.index >= nodes_.size()) {
        return nullptr;
    }
    Node *node = nodes_[id.index];
    return node && node->id == id ? node : nullptr;
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
    return root_ ? root_->id : contracts::NodeId{};
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
        if (!scene_detail::ValidPropertyValue(target.property, value) ||
            (target.property == DslProperty::Text &&
             !scene_detail::ValidEditorText(target.node->kind, value))) {
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
        if (!scene_detail::ValidPropertyValue(target.property, value) ||
            (target.property == DslProperty::Text &&
             !scene_detail::ValidEditorText(target.node->kind, value))) {
            return false;
        }
    }
    const auto now = AnimationNowNs();
    bool changed = false;
    for (const auto &target : it->second) {
        changed = SetPropertyAt(target.node->id, target.property, value, now) || changed;
    }
    return changed;
}

bool Scene::SetProperty(contracts::NodeId id, DslProperty property, PropertyValue value)
{
    return SetPropertyAt(id, property, std::move(value), AnimationNowNs());
}

bool Scene::SetPropertyAt(contracts::NodeId id, DslProperty property, PropertyValue value,
                          std::uint64_t now)
{
    Node *node = Find(id);
    if (!node || property == DslProperty::Material ||
        !(node->allowed_properties & PropertyBit(property)) ||
        (node->decorative &&
         (property == DslProperty::Action || property == DslProperty::BackdropBlur ||
          property == DslProperty::InputShape)) ||
        (property >= DslProperty::TranslateX && node->kind != Kind::Visual) ||
        !scene_detail::ValidPropertyValue(property, value)) {
        return false;
    }
    if (property == DslProperty::Text && !scene_detail::ValidEditorText(node->kind, value)) {
        return false;
    }
    const auto previous = CurrentProperty(*node, property);
    if (previous == value) {
        return false;
    }
    if (property == DslProperty::Action) {
        PrepareInputGeometry();
        input_snapshot_dirty_ = true;
    }
    const bool was_visible = IsVisible(*node);
    const bool state_property = IsStateProperty(*node, property);
    const bool animated =
        !state_property && RetargetPresentation(*node, property, previous, value, now);
    node->properties[property] = value;
    node->explicit_properties.insert(property);
    std::erase_if(node->theme_refs,
                  [property](const ThemeRef &ref) { return ref.target == property; });
    ApplyCachedProperty(*node, property, value);
    if (state_property) {
        ResolveNodeStateTargets(*node, now, true);
    }
    if (property == DslProperty::Source) {
        node->image_ready = false;
        node->intrinsic_size = {};
    }
    ++transaction_revision_;
    // Retain all state while concealed, including cache revisions and resolved
    // style overrides. Revealing any ancestor triggers a full layout/snapshot.
    // A child becoming locally visible under a hidden parent stays concealed.
    if (!was_visible && !IsVisible(*node)) {
        ++node->revision;
        return true;
    }
    if (property == DslProperty::Visible && !node->style.visible) {
        CancelHiddenAnimations();
    }
    if (property == DslProperty::Visible) {
        ReconcileInput();
    } else if (property == DslProperty::Action) {
        RefreshInputGeometry();
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
    if (property == DslProperty::Radius || property == DslProperty::Clip ||
        property == DslProperty::Overflow || property == DslProperty::InputShape) {
        hit_geometry_dirty_ = true;
    }
    if (!animated) {
        ++node->revision;
        Invalidate(affected |
                   ((input_shape && affected == Dirty::None) || property == DslProperty::Action
                        ? Dirty::Composite
                        : Dirty::None));
    }
    return true;
}

bool Scene::SetViewport(contracts::LogicalSize size)
{
    if (!scene_detail::ValidSize(size)) {
        return false;
    }
    if (viewport_.width != size.width || viewport_.height != size.height) {
        viewport_ = size;
        ++transaction_revision_;
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
        if (!node || node->kind != Kind::Image || node->image != image) {
            continue;
        }
        if (!node->image_ready || node->intrinsic_size.width != intrinsic_size.width ||
            node->intrinsic_size.height != intrinsic_size.height) {
            const bool affects_layout = node->style.width <= 0 || node->style.height <= 0;
            node->intrinsic_size = intrinsic_size;
            node->image_ready = true;
            input_dirty_ = true;
            ++node->revision;
            if (node && IsVisible(*node)) {
                Invalidate(Dirty::Paint | (affects_layout ? Dirty::Layout : Dirty::None));
            }
            changed = true;
        }
    }
    return changed;
}

std::optional<contracts::DisplayList> Scene::Build(contracts::WindowId window)
{
    ResolveInteractionStyles();
    ++build_calls_;
    if (!root_ || !window || !scene_detail::ValidSize(viewport_)) {
        return std::nullopt;
    }
    input_snapshot_dirty_ = input_snapshot_dirty_ || hit_geometry_dirty_;
    if (!Has(dirty_, Dirty::Layout) && !Has(dirty_, Dirty::Paint)) {
        UpdateInputSnapshot();
        return std::nullopt;
    }

    if (hit_geometry_dirty_) {
        PrepareInputGeometry();
    }
    SceneSnapshot snapshot;
    if (theme_) {
        snapshot.controls = theme_->controls;
    }
    snapshot.root = root_->id;
    snapshot.nodes.resize(nodes_.size());
    for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
        snapshot.nodes[i].id = {static_cast<std::uint32_t>(i), 0};
        snapshot.nodes[i].style.visible = false;
    }
    for (const Node *node : nodes_) {
        if (!node) {
            continue;
        }
        SnapshotNode item;
        item.id = node->id;
        item.kind = node->kind;
        item.style = node->style;
        item.text = node->text;
        item.icon = node->icon;
        item.value = node->value;
        item.checked = node->checked;
        item.interaction = State(node->id);
        item.presentation = node->presentation;
        item.image = node->image;
        item.intrinsic_size = node->intrinsic_size;
        item.image_ready = node->image_ready;
        item.bounds = node->bounds;
        item.shaped = node->shaped;
        item.revision = node->revision;
        ApplyPresentation(*node, item);
        for (const auto &child : node->children) {
            item.children.push_back(child->id);
        }
        snapshot.nodes[node->id.index] = std::move(item);
    }
    if (Has(dirty_, Dirty::Layout)) {
        LayoutEngine::Compute(snapshot, viewport_, shaper_);
        ++layout_count_;
        input_dirty_ = true;
        for (const auto &item : snapshot.nodes) {
            if (!item.id) {
                continue;
            }
            Node *node = nodes_[item.id.index];
            node->bounds = item.bounds;
            node->shaped = item.shaped;
        }
    }

    if (hit_geometry_dirty_) {
        RefreshInputGeometry();
        ResolveInteractionStyles();
        hit_geometry_dirty_ = false;
        for (auto &item : snapshot.nodes) {
            if (const auto *node = Find(item.id)) {
                item.interaction = State(item.id);
                item.revision = node->revision;
                ApplyPresentation(*node, item);
            }
        }
    }

    for (auto &item : snapshot.nodes) {
        if (auto *node = Find(item.id);
            node && IsVisible(*node) &&
            (node->kind == Kind::TextField || node->kind == Kind::TextArea)) {
            PrepareTextVisual(*node, item);
        }
    }
    UpdateInputSnapshot();

    auto next = RenderTreeBuilder::Build(snapshot, render_tree_.get());
    auto list = DisplayListBuilder::Build(next, window, font_, generation_ + 1);
    render_tree_ = std::make_unique<RenderTree>(std::move(next));
    ++generation_;

    dirty_ = Has(dirty_, Dirty::Composite) ? Dirty::Composite : Dirty::None;
    return list;
}

} // namespace prism::runtime
