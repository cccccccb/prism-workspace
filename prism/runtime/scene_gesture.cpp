#include "scene_p.hpp"
#include <atomic>
#include <limits>
#include <stdexcept>

namespace prism::runtime {
namespace {
std::uint64_t NextGestureId()
{
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value < std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
    throw std::overflow_error("Gesture identity exhausted");
}
} // namespace

bool Scene::SetGesture(contracts::NodeId id, std::optional<GestureSpec> spec)
{
    auto *node = Find(id);
    if (!node || node->kind != Kind::InteractionTarget || (spec && !ValidGestureSpec(*spec)) ||
        node->gesture == spec) {
        return false;
    }

    node->gesture.swap(spec);
    input_snapshot_dirty_ = true;
    ++transaction_revision_;
    ReconcileInput();
    ResolveInteractionStyles();
    Invalidate(Dirty::Composite);
    return true;
}

std::uint64_t Scene::StartGesture(contracts::NodeId id, contracts::InputSource source, bool touch,
                                  contracts::InputContactId contact, std::uint32_t serial,
                                  contracts::LogicalPoint point, std::uint64_t time_ns,
                                  const std::shared_ptr<const InputSnapshot> &snapshot)
{
    const auto *node = Find(id);
    if (!node || !node->gesture) {
        return 0;
    }
    // Bound retained streams even when a direct Scene consumer does not drain.
    // Reject rather than silently converting an overflowing drag into a click.
    if (input_state_->gestures.size() >= 256) {
        throw std::length_error("Undrained gesture stream limit exceeded");
    }

    InputState::Gesture gesture;
    gesture.event = {contracts::GesturePhase::Begin,
                     NextGestureId(),
                     id,
                     node->gesture->action,
                     source,
                     touch,
                     contact,
                     serial,
                     point,
                     point,
                     time_ns,
                     snapshot ? snapshot->scene : input_scene_id_,
                     snapshot ? snapshot->version : 0};
    gesture.spec = *node->gesture;
    gesture.click_action = node->action;
    if (gesture.spec.threshold == 0) {
        gesture.dragging = true;
        gesture.begin_pending = true;
        gesture.begin_position = point;
        gesture.begin_time_ns = time_ns;
    }
    const auto result = gesture.event.id;
    input_state_->gestures.push_back(std::move(gesture));
    return result;
}

void Scene::MoveGesture(std::uint64_t id, contracts::LogicalPoint point,
                        std::uint64_t time_ns) noexcept
{
    if (!id || !std::isfinite(point.x) || !std::isfinite(point.y)) {
        return;
    }
    for (auto &gesture : input_state_->gestures) {
        if (gesture.event.id != id || gesture.terminal) {
            continue;
        }
        const auto previous = gesture.event.position;
        gesture.event.position = point;
        gesture.event.time_ns = std::max(time_ns, gesture.event.time_ns);
        if (!gesture.dragging &&
            std::hypot(point.x - gesture.event.start.x, point.y - gesture.event.start.y) >=
                gesture.spec.threshold) {
            gesture.dragging = true;
            gesture.begin_pending = true;
            gesture.begin_position = point;
            gesture.begin_time_ns = gesture.event.time_ns;
        } else if (gesture.dragging && point != previous) {
            gesture.update_pending = true;
            gesture.update_time_ns = gesture.event.time_ns;
        }
        return;
    }
}

bool Scene::FinishGesture(std::uint64_t id, contracts::GesturePhase phase,
                          std::uint64_t time_ns) noexcept
{
    for (auto &gesture : input_state_->gestures) {
        if (gesture.event.id != id) {
            continue;
        }
        if (!gesture.terminal) {
            gesture.event.time_ns = std::max(time_ns, gesture.event.time_ns);
            gesture.terminal = phase;
        }
        return gesture.dragging;
    }
    return false;
}

bool Scene::IsDragging(std::uint64_t id) const noexcept
{
    return id && std::any_of(input_state_->gestures.begin(), input_state_->gestures.end(),
                             [id](const InputState::Gesture &gesture) {
                                 return gesture.event.id == id && gesture.dragging &&
                                        !gesture.terminal;
                             });
}

bool Scene::IsGestureActive(std::uint64_t id) const noexcept
{
    return !id || std::any_of(input_state_->gestures.begin(), input_state_->gestures.end(),
                              [id](const InputState::Gesture &gesture) {
                                  return gesture.event.id == id && !gesture.terminal;
                              });
}

void Scene::ReconcileGestures() noexcept
{
    for (auto &gesture : input_state_->gestures) {
        if (gesture.terminal) {
            continue;
        }
        const auto *node = Find(gesture.event.node);
        if (!IsInteractive(gesture.event.node) || !node->gesture ||
            *node->gesture != gesture.spec || node->action != gesture.click_action) {
            gesture.terminal = contracts::GesturePhase::Cancel;
        }
    }
}

std::vector<contracts::GestureEvent> Scene::TakeGestureEvents()
{
    std::vector<contracts::GestureEvent> result;
    result.reserve(input_state_->gestures.size() * 3);
    for (const auto &gesture : input_state_->gestures) {
        if (!gesture.dragging) {
            continue;
        }
        if (gesture.begin_pending) {
            auto event = gesture.event;
            event.phase = contracts::GesturePhase::Begin;
            event.position = gesture.begin_position;
            event.time_ns = gesture.begin_time_ns;
            result.push_back(std::move(event));
        }
        if (gesture.update_pending) {
            auto event = gesture.event;
            event.phase = contracts::GesturePhase::Update;
            event.time_ns = gesture.update_time_ns;
            result.push_back(std::move(event));
        }
        if (gesture.terminal) {
            auto event = gesture.event;
            event.phase = *gesture.terminal;
            result.push_back(std::move(event));
        }
    }

    // No allocation in cancellation/reconciliation, including transaction commit.
    // Publish no partial drain if copying an owning event throws above.
    for (auto &gesture : input_state_->gestures) {
        gesture.begin_pending = false;
        gesture.update_pending = false;
    }
    std::erase_if(input_state_->gestures,
                  [](const InputState::Gesture &gesture) { return gesture.terminal.has_value(); });
    return result;
}
} // namespace prism::runtime
