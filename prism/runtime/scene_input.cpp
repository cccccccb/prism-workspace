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
           (!node->action.empty() || node->kind == Kind::InteractionTarget);
}

InteractionState Scene::State(contracts::NodeId id) const
{
    const auto *node = Find(id);
    if (node) {
        auto state = node->interaction;
        state.enabled = IsEnabled(*node);
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
    node->enabled = enabled;
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
    ResolveInteractionStyles();
    Invalidate(Dirty::Paint);
    return true;
}

std::optional<HitResult> Scene::Hit(const Node &node, contracts::LogicalPoint point) const
{
    if (!node.style.visible || !node.enabled || node.kind == Kind::Visual) {
        return std::nullopt;
    }
    const bool inside = scene_detail::Inside(node.bounds, point);
    const bool clip = node.style.clip || node.style.overflow == "clip";
    if (!inside && clip) {
        return std::nullopt;
    }

    const double radius =
        std::min({node.style.radius, node.bounds.width / 2, node.bounds.height / 2});
    bool rounded_inside = inside;
    if (radius > 0 && inside) {
        const double cx =
            std::clamp(point.x, node.bounds.x + radius, node.bounds.x + node.bounds.width - radius);
        const double cy = std::clamp(point.y, node.bounds.y + radius,
                                     node.bounds.y + node.bounds.height - radius);
        rounded_inside =
            (point.x - cx) * (point.x - cx) + (point.y - cy) * (point.y - cy) <= radius * radius;
    }
    if (clip && !rounded_inside) {
        return std::nullopt;
    }

    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        if (auto hit = Hit(**it, point)) {
            return hit;
        }
    }
    if (rounded_inside && (!node.action.empty() || node.kind == Kind::InteractionTarget)) {
        return HitResult{node.id, {point.x - node.bounds.x, point.y - node.bounds.y}};
    }
    return std::nullopt;
}

std::optional<HitResult> Scene::HitTest(contracts::LogicalPoint point) const
{
    if (!root_ || !std::isfinite(point.x) || !std::isfinite(point.y)) {
        return std::nullopt;
    }
    return Hit(*root_, point);
}

std::optional<std::string> Scene::ActionAt(contracts::LogicalPoint point) const
{
    const auto hit = HitTest(point);
    const auto *node = hit ? Find(hit->node) : nullptr;
    return node && !node->action.empty() ? std::optional<std::string>(node->action) : std::nullopt;
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
        for (const auto &pointer : input_state_->pointers) {
            state.hovered = state.hovered || pointer.hovered == id;
            state.captured = state.captured || pointer.captured == id;
            state.dragging =
                state.dragging || (pointer.captured == id && IsDragging(pointer.gesture));
            state.pressed = state.pressed || (pointer.captured == id && pointer.hovered == id);
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
    ReconcileGestures();
    for (auto &pointer : input_state_->pointers) {
        if (!IsInteractive(pointer.hovered) ||
            (pointer.submitted && !IsInteractive(pointer.hovered, pointer.snapshot.get()))) {
            pointer.hovered = {};
        }
        const auto *captured = Find(pointer.captured);
        if (!IsInteractive(pointer.captured) || captured->action != pointer.action ||
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
        return !IsInteractive(key.node) || node->action != key.action;
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
    for (auto &gesture : input_state_->gestures) {
        FinishGesture(gesture.event.id, contracts::GesturePhase::Cancel);
    }
    input_state_->pointers.clear();
    input_state_->focus.clear();
    input_state_->keys.clear();
    input_state_->touches.clear();
    return RefreshInputStates();
}

void Scene::CancelSeatInput(std::uint64_t seat) noexcept
{
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
    std::erase_if(input_state_->keys,
                  [seat](const InputState::KeyPress &key) { return key.source.seat == seat; });
    if (current == focus.end()) {
        focus.push_back({seat, id, visible});
    } else {
        *current = {seat, id, visible};
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
    std::erase_if(order, [this, snapshot, submitted](const Node *node) {
        return !IsInteractive(node->id) || node->action.empty() ||
               (submitted && !IsInteractive(node->id, snapshot));
    });
    if (order.empty()) {
        return false;
    }
    const auto focus =
        std::find_if(input_state_->focus.begin(), input_state_->focus.end(),
                     [seat](const InputState::Focus &item) { return item.seat == seat; });
    const auto current = focus == input_state_->focus.end() ? contracts::NodeId{} : focus->node;
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
    return Find(focus->node)->action;
}

bool Scene::SetPointer(contracts::LogicalPoint point)
{
    MoveInputPointer({}, point);
    return RefreshInputStates();
}
} // namespace prism::runtime
