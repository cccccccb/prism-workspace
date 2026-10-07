#include "prism/contracts/rounded_region.hpp"
#include "scene_contour_p.hpp"
#include "scene_p.hpp"

namespace prism::runtime {
namespace {
bool HasState(const InteractionState &state)
{
    return state.hovered || state.pressed || state.captured || state.focused || state.dragging;
}

void AddTarget(std::vector<contracts::NodeId> &targets, contracts::NodeId id)
{
    if (id && std::find(targets.begin(), targets.end(), id) == targets.end()) {
        targets.push_back(id);
    }
}
} // namespace

bool Scene::IsEnabled(const Node &node) const
{
    for (const Node *current = &node; current; current = current->parent) {
        if (!current->enabled) {
            return false;
        }
    }
    return true;
}

bool Scene::IsInteractive(contracts::NodeId id) const
{
    const auto *node = Find(id);
    return node && IsVisible(*node) && IsEnabled(*node) &&
           (!IsChoiceOption(node->kind) || !node->parent->selected_key.empty()) &&
           (!InputAction(*node).empty() || node->kind == Kind::InteractionTarget ||
            (IsPopupKind(node->kind) || node->kind == Kind::ScrollView));
}

InteractionState Scene::State(contracts::NodeId id) const
{
    const auto *node = Find(id);
    if (node) {
        auto state = node->interaction;
        state.enabled = IsEnabled(*node);
        state.selected = Selected(*node);
        return state;
    }
    InteractionState state;
    state.enabled = false;
    return state;
}

bool Scene::SetEnabled(contracts::NodeId id, bool enabled)
{
    auto *node = Find(id);
    if (!node || node->enabled == enabled) {
        return false;
    }
    PrepareInputGeometry();
    node->properties[DslProperty::Enabled] = enabled;
    node->explicit_properties.insert(DslProperty::Enabled);
    std::erase_if(node->theme_refs,
                  [](const ThemeRef &ref) { return ref.target == DslProperty::Enabled; });
    node->enabled = enabled;
    ++node->revision;
    input_snapshot_dirty_ = true;
    state_styles_dirty_ = true;
    ++transaction_revision_;

    for (auto *current : nodes_) {
        if (!current || current->interaction.enabled == IsEnabled(*current)) {
            continue;
        }
        current->interaction.enabled = IsEnabled(*current);
        ++current->revision;
    }
    RefreshInputGeometry();
    ReconcilePopup();
    ResolveInteractionStyles();
    Invalidate(Dirty::Paint);
    return true;
}

std::optional<HitResult> Scene::Hit(const Node &node, contracts::LogicalPoint point) const
{
    if (!IsVisible(node) || !node.enabled || node.kind == Kind::Visual ||
        (HasPopupSurfaceAdoption() && node.id == active_popup_)) {
        return std::nullopt;
    }
    const bool clip = IsPopupKind(node.kind) || node.kind == Kind::ScrollView || node.style.clip ||
                      node.style.overflow == "clip";
    const bool rounded_inside =
        SceneShapeContains({{node.bounds, node.style.radius}, node.contour}, point);
    if (clip && !rounded_inside) {
        return std::nullopt;
    }

    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        if (auto hit = Hit(**it, point)) {
            return hit;
        }
    }
    if (rounded_inside &&
        (!InputAction(node).empty() || (node.kind == Kind::InteractionTarget ||
                                        node.kind == Kind::ScrollView || IsPopupKind(node.kind))) &&
        IsInteractive(node.id)) {
        return HitResult{node.id, {point.x - node.bounds.x, point.y - node.bounds.y}};
    }
    return std::nullopt;
}

std::optional<HitResult> Scene::HitTest(contracts::LogicalPoint point) const
{
    if (!root_ || !std::isfinite(point.x) || !std::isfinite(point.y) ||
        !scene_detail::Inside({0, 0, viewport_.width, viewport_.height}, point)) {
        return std::nullopt;
    }
    return Hit(*root_, point);
}

std::optional<std::string> Scene::ActionAt(contracts::LogicalPoint point) const
{
    const auto hit = HitTest(point);
    const auto *node = hit ? Find(hit->node) : nullptr;
    return node && !InputAction(*node).empty() ? std::optional<std::string>(InputAction(*node))
                                               : std::nullopt;
}

void Scene::TrackInputTarget(contracts::NodeId id)
{
    // Reserve an identity before publishing an input reference to it. Lifecycle
    // reconciliation only removes references and must not allocate after commit.
    AddTarget(input_state_->active, id);
}

bool Scene::RefreshInputStates() noexcept
{
    auto &targets = input_state_->active;
    bool changed = false;
    for (const auto id : targets) {
        auto *node = Find(id);
        if (!node) {
            continue;
        }
        InteractionState state;
        state.enabled = IsEnabled(*node);
        state.selected = Selected(*node);
        for (const auto &pointer : input_state_->pointers) {
            state.hovered = state.hovered || pointer.hovered == id;
            state.captured = state.captured || pointer.captured == id;
            state.dragging =
                state.dragging || (pointer.captured == id && IsDragging(pointer.gesture));
            state.pressed = state.pressed || (pointer.captured == id && pointer.hovered == id);
        }
        for (const auto &slider : input_state_->sliders) {
            if (slider.node == id && !slider.terminal) {
                state.pressed = true;
                state.captured = state.captured || slider.key == 0;
                state.dragging = state.dragging || slider.key == 0;
            }
        }
        for (const auto &focus : input_state_->focus) {
            state.focused = state.focused || focus.node == id;
            state.focusVisible = state.focusVisible || (focus.node == id && focus.visible);
        }
        for (const auto &key : input_state_->keys) {
            state.pressed = state.pressed || key.node == id;
        }
        for (const auto &touch : input_state_->touches) {
            state.captured = state.captured || touch.captured == id;
            state.dragging = state.dragging || (touch.captured == id && IsDragging(touch.gesture));
            state.pressed = state.pressed || (touch.captured == id && touch.inside);
        }
        if (node->interaction != state) {
            node->interaction = state;
            ++node->revision;
            changed = true;
        }
    }
    std::erase_if(targets, [this](contracts::NodeId id) {
        const auto *node = Find(id);
        return !node || !HasState(node->interaction);
    });
    if (changed) {
        state_styles_dirty_ = true;
        Invalidate(Dirty::Paint);
    }
    return changed;
}

bool Scene::ReconcileInput() noexcept
{
    ReconcileSliders();
    ReconcileGestures();
    for (auto &pointer : input_state_->pointers) {
        if (!IsInteractive(pointer.hovered) ||
            (pointer.submitted && !IsInteractive(pointer.hovered, pointer.snapshot.get()))) {
            pointer.hovered = {};
        }
        const auto *captured = Find(pointer.captured);
        if (!IsInteractive(pointer.captured) || InputAction(*captured) != pointer.action ||
            (IsControlTarget(captured->kind) &&
             ControlRevision(*captured) != pointer.control_revision) ||
            !IsGestureActive(pointer.gesture)) {
            FinishGesture(pointer.gesture, contracts::GesturePhase::Cancel);
            pointer.gesture = 0;
            pointer.captured = {};
            pointer.action.clear();
        }
    }
    std::erase_if(input_state_->pointers, [](const InputState::Pointer &pointer) {
        return !pointer.inside && !pointer.captured;
    });
    std::erase_if(input_state_->focus,
                  [this](const InputState::Focus &focus) { return !IsInteractive(focus.node); });
    std::erase_if(input_state_->keys, [this](const InputState::KeyPress &key) {
        const auto *node = Find(key.node);
        return !IsInteractive(key.node) || InputAction(*node) != key.action ||
               (IsControlTarget(node->kind) && ControlRevision(*node) != key.control_revision);
    });
    std::erase_if(input_state_->touches, [this](const InputState::Touch &touch) {
        const auto *node = Find(touch.captured);
        return !IsInteractive(touch.captured) || node->action != touch.action ||
               !IsGestureActive(touch.gesture);
    });
    return RefreshInputStates();
}

void Scene::PrepareInputGeometry()
{
    // Layout may move one new target under each stationary pointer. Reserve
    // before changing geometry so refreshing those targets cannot allocate.
    input_state_->active.reserve(input_state_->active.size() + input_state_->pointers.size());
}

void Scene::RefreshInputGeometry() noexcept
{
    for (auto &pointer : input_state_->pointers) {
        if (!pointer.inside) {
            continue;
        }
        const auto hit = InputHit(pointer.position, pointer.snapshot, pointer.submitted);
        pointer.hovered = hit ? hit->node : contracts::NodeId{};
        TrackInputTarget(pointer.hovered);
    }
    RefreshTouchGeometry();
    ReconcileInput();
}

bool Scene::CancelInput()
{
    const auto previous_pixels = pixels_revision_;
    for (std::size_t i = 0; i < input_state_->sliders.size(); ++i) {
        FinishSlider(i, ValueCancelReason::Unavailable);
    }
    for (auto &gesture : input_state_->gestures) {
        FinishGesture(gesture.event.id, contracts::GesturePhase::Cancel);
    }
    input_state_->pointers.clear();
    input_state_->focus.clear();
    input_state_->keys.clear();
    input_state_->touches.clear();
    return RefreshInputStates() || pixels_revision_ != previous_pixels;
}

void Scene::CancelSeatInput(std::uint64_t seat) noexcept
{
    for (std::size_t i = 0; i < input_state_->sliders.size(); ++i) {
        if (input_state_->sliders[i].source.seat == seat) {
            FinishSlider(i, ValueCancelReason::FocusLost);
        }
    }
    for (auto &gesture : input_state_->gestures) {
        if (gesture.event.source.seat == seat) {
            FinishGesture(gesture.event.id, contracts::GesturePhase::Cancel);
        }
    }
    std::erase_if(input_state_->pointers, [seat](const InputState::Pointer &pointer) {
        return pointer.source.seat == seat;
    });
    std::erase_if(input_state_->focus,
                  [seat](const InputState::Focus &focus) { return focus.seat == seat; });
    std::erase_if(input_state_->keys,
                  [seat](const InputState::KeyPress &key) { return key.source.seat == seat; });
    std::erase_if(input_state_->touches,
                  [seat](const InputState::Touch &touch) { return touch.source.seat == seat; });
}

bool Scene::SetInputFocus(contracts::NodeId id, std::uint64_t seat, bool visible)
{
    auto &focus = input_state_->focus;
    auto current = std::find_if(focus.begin(), focus.end(), [seat](const InputState::Focus &item) {
        return item.seat == seat;
    });
    if (current != focus.end() && current->node == id && current->visible == visible) {
        return false;
    }
    TrackInputTarget(id);
    for (std::size_t i = 0; i < input_state_->sliders.size(); ++i) {
        const auto &slider = input_state_->sliders[i];
        if (slider.source.seat == seat && slider.node != id) {
            FinishSlider(i, ValueCancelReason::FocusLost);
        }
    }
    std::erase_if(input_state_->keys,
                  [seat](const InputState::KeyPress &key) { return key.source.seat == seat; });
    if (current == focus.end()) {
        focus.push_back({seat, id, visible});
    } else {
        *current = {seat, id, visible};
    }
    if (visible) {
        RevealScrollTarget(id);
    }
    return true;
}

bool Scene::MoveInputFocus(std::uint64_t seat, bool reverse, const InputSnapshot *snapshot,
                           bool submitted)
{
    std::vector<Node *> order;
    if (root_) {
        CollectNodes(*root_, order);
    }
    std::vector<Node *> stops;
    std::set<const Node *> groups;
    for (auto *node : order) {
        if (!InPopupScope(*node)) {
            continue;
        }
        if (IsChoiceOption(node->kind)) {
            if (groups.insert(node->parent).second) {
                if (auto *stop = ChoiceTabStop(*node->parent, snapshot, submitted)) {
                    stops.push_back(stop);
                }
            }
        } else if (IsInteractive(node->id) && !InputAction(*node).empty() &&
                   (!submitted || IsInteractive(node->id, snapshot))) {
            stops.push_back(node);
        }
    }
    order.swap(stops);
    if (order.empty()) {
        return false;
    }
    const auto focus =
        std::find_if(input_state_->focus.begin(), input_state_->focus.end(),
                     [seat](const InputState::Focus &item) { return item.seat == seat; });
    auto current = focus == input_state_->focus.end() ? contracts::NodeId{} : focus->node;
    if (const auto *node = Find(current); node && IsChoiceOption(node->kind)) {
        const auto *stop = ChoiceTabStop(*node->parent, snapshot, submitted);
        current = stop ? stop->id : contracts::NodeId{};
    }
    const auto position = std::find_if(order.begin(), order.end(),
                                       [current](const Node *node) { return node->id == current; });
    Node *next = reverse ? order.back() : order.front();
    if (position != order.end()) {
        if (reverse) {
            next = position == order.begin() ? order.back() : *std::prev(position);
        } else {
            next = std::next(position) == order.end() ? order.front() : *std::next(position);
        }
    }
    return SetInputFocus(next->id, seat, true);
}

bool Scene::FocusNext()
{
    MoveInputFocus(0, false);
    return RefreshInputStates();
}

std::optional<std::string> Scene::FocusedAction() const
{
    const auto focus = std::find_if(input_state_->focus.begin(), input_state_->focus.end(),
                                    [](const InputState::Focus &item) { return item.seat == 0; });
    if (focus == input_state_->focus.end() || !IsInteractive(focus->node)) {
        return std::nullopt;
    }
    return std::string(InputAction(*Find(focus->node)));
}

bool Scene::SetPointer(contracts::LogicalPoint point)
{
    MoveInputPointer({}, point);
    return RefreshInputStates();
}
} // namespace prism::runtime
