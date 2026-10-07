#include "prism/runtime/control_value_delivery.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
bool Finite(const ControlValue &value)
{
    const auto *number = std::get_if<double>(&value);
    return !number || std::isfinite(*number);
}

struct DeliveryGuard {
    bool &active;

    ~DeliveryGuard()
    {
        active = false;
    }
};
} // namespace

void ControlValueDelivery::SetHandler(Handler handler)
{
    Clear();
    handler_ = std::move(handler);
}

void ControlValueDelivery::Clear() noexcept
{
    pending_.clear();
    previews_.clear();
}

void ControlValueDelivery::Enqueue(UiLoadId ui, ControlEdit edit)
{
    if (!ui.owner || !ui.generation || !edit.node || edit.action.empty() ||
        !edit.event.interaction || edit.event.before.index() != edit.event.value.index() ||
        !Finite(edit.event.before) || !Finite(edit.event.value)) {
        throw std::invalid_argument("Invalid control delivery envelope");
    }
    const auto phase = edit.event.phase;
    const auto reason = edit.event.reason;
    if (reason < ValueCancelReason::None || reason > ValueCancelReason::Superseded) {
        throw std::invalid_argument("Invalid control cancellation reason");
    }
    if ((phase != ValuePhase::Preview && phase != ValuePhase::Commit &&
         phase != ValuePhase::Cancel) ||
        (phase == ValuePhase::Cancel ? reason == ValueCancelReason::None
                                     : reason != ValueCancelReason::None)) {
        throw std::invalid_argument("Invalid control delivery phase/reason");
    }
    if (!handler_) {
        return;
    }
    if (pending_.size() >= 1024) {
        throw std::length_error("Control callback queue limit exceeded");
    }

    pending_.push_back({ui, std::move(edit)});
}

bool ControlValueDelivery::SameTarget(const Pending &a, const Pending &b)
{
    return a.ui == b.ui && a.edit.node == b.edit.node;
}

bool ControlValueDelivery::SameInteraction(const Pending &a, const Pending &b)
{
    return SameTarget(a, b) && a.edit.event.interaction == b.edit.event.interaction;
}

void ControlValueDelivery::Cancel(std::size_t index, ValueCancelReason reason)
{
    auto pending = std::move(previews_[index]);
    previews_.erase(previews_.begin() + index);
    pending.edit.event.phase = ValuePhase::Cancel;
    pending.edit.event.reason = reason;
    pending.edit.event.value = pending.edit.event.before;

    // Finish bookkeeping before user code can re-enter or replace the callback.
    auto callback = handler_;
    if (callback) {
        callback(pending.edit);
    }
}

bool ControlValueDelivery::Reconcile(const Validator &validate)
{
    for (std::size_t i = 0; i < previews_.size(); ++i) {
        const auto reason = validate(previews_[i].ui, previews_[i].edit);
        if (reason != ValueCancelReason::None) {
            Cancel(i, reason);
            return true;
        }
    }
    return false;
}

void ControlValueDelivery::Deliver(Pending pending, const Validator &validate)
{
    auto active = std::find_if(previews_.begin(), previews_.end(), [&pending](const Pending &item) {
        return SameInteraction(item, pending);
    });
    if (pending.edit.event.phase == ValuePhase::Cancel) {
        // Cancellation belongs to the observed preview, even after UI/value retirement.
        if (active != previews_.end()) {
            Cancel(static_cast<std::size_t>(active - previews_.begin()), pending.edit.event.reason);
        }
        return;
    }
    if (validate(pending.ui, pending.edit) != ValueCancelReason::None) {
        return;
    }

    if (active != previews_.end() && (active->edit.action != pending.edit.action ||
                                      active->edit.event.revision != pending.edit.event.revision ||
                                      active->edit.event.before != pending.edit.event.before)) {
        throw std::invalid_argument("Control interaction changed its origin");
    }
    if (active == previews_.end()) {
        const auto previous =
            std::find_if(previews_.begin(), previews_.end(),
                         [&pending](const Pending &item) { return SameTarget(item, pending); });
        if (previous != previews_.end()) {
            if (pending.edit.event.interaction < previous->edit.event.interaction) {
                return;
            }
            // Retry only after cancellation; its callback may replace the UI or receiver.
            pending_.push_front(std::move(pending));
            Cancel(static_cast<std::size_t>(previous - previews_.begin()),
                   ValueCancelReason::Superseded);
            return;
        }
    }
    if (pending.edit.event.phase == ValuePhase::Preview) {
        if (active != previews_.end()) {
            if (active->edit.event.value == pending.edit.event.value) {
                return;
            }
            *active = pending;
        } else {
            if (previews_.size() >= 256) {
                throw std::length_error("Active control preview limit exceeded");
            }
            previews_.push_back(pending);
        }
    } else if (active != previews_.end()) {
        previews_.erase(active);
    }

    auto callback = handler_;
    if (callback) {
        callback(pending.edit);
    }
}

void ControlValueDelivery::Drain(const Validator &validate)
{
    if (delivering_) {
        return;
    }
    if (!validate) {
        throw std::invalid_argument("Control delivery needs a validator");
    }
    DeliveryGuard guard{delivering_};
    delivering_ = true;

    try {
        std::size_t turns = 0;
        for (;;) {
            if (++turns > 4096) {
                throw std::length_error("Control callback delivery turn limit exceeded");
            }
            if (Reconcile(validate)) {
                continue;
            }
            if (pending_.empty()) {
                break;
            }
            auto pending = std::move(pending_.front());
            pending_.pop_front();
            Deliver(std::move(pending), validate);
        }
    } catch (...) {
        Clear();
        throw;
    }
}

} // namespace prism::runtime
