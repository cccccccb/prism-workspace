#include "prism/sdk/layout_control_bridge.hpp"
#include <cmath>
#include <limits>
#include <utility>

namespace prism::sdk {
using namespace contracts;

LayoutControlBridge::LayoutControlBridge(Send send) : send_(std::move(send))
{
}

void LayoutControlBridge::Observe(const GestureEvent &event)
{
    observed_ = event;
    if (auto it = controls_.find(event.id); it != controls_.end() && !it->second.terminal) {
        it->second.latest = event;
    }
}

bool LayoutControlBridge::Begin(std::uint64_t gesture, LayoutControlOperation operation,
                                const LayoutControlTarget &target)
{
    if (disconnected_ || !send_ || !observed_ || observed_->id != gesture || !gesture ||
        observed_->phase != GesturePhase::Begin || controls_.contains(gesture) ||
        controls_.size() >= 16) {
        return false;
    }

    Control control;
    control.latest = *observed_;
    control.last.gesture = gesture;
    control.last.operation = operation;
    control.last.target = target;
    control.last.input = {observed_->touch ? LayoutInputKind::Touch : LayoutInputKind::Pointer,
                          observed_->serial, observed_->touch ? observed_->contact : 0};
    if (!Submit(control, LayoutControlPhase::Begin, observed_->position)) {
        return false;
    }
    controls_.emplace(gesture, std::move(control));
    return true;
}

bool LayoutControlBridge::EndIntent(std::uint64_t gesture, LayoutControlIntent intent)
{
    const auto it = controls_.find(gesture);
    if (!observed_ || observed_->id != gesture || observed_->phase != GesturePhase::End ||
        it == controls_.end() || it->second.terminal ||
        intent > LayoutControlIntent::SplitVertical) {
        return false;
    }
    if (!AllowsLayoutIntent(it->second.last.operation, intent)) {
        return false;
    }

    it->second.intent = intent;
    return true;
}

bool LayoutControlBridge::Cancel(std::uint64_t gesture)
{
    const auto it = controls_.find(gesture);
    if (it == controls_.end() || it->second.terminal) {
        return false;
    }

    auto terminal = it->second.latest;
    terminal.phase = GesturePhase::Cancel;
    it->second.terminal = std::move(terminal);
    it->second.update.reset();
    Pump(gesture);
    return true;
}

void LayoutControlBridge::Advance()
{
    if (!observed_) {
        return;
    }
    const auto event = std::exchange(observed_, std::nullopt);
    const auto it = controls_.find(event->id);
    if (it == controls_.end() || it->second.terminal) {
        return;
    }

    if (event->phase == GesturePhase::Update) {
        it->second.update = *event;
    } else if (event->phase == GesturePhase::End || event->phase == GesturePhase::Cancel) {
        it->second.terminal = *event;
        if (event->phase == GesturePhase::Cancel) {
            it->second.update.reset();
        }
    }
    Pump(event->id);
}

bool LayoutControlBridge::Submit(Control &control, LayoutControlPhase phase, LogicalPoint position)
{
    if (!next_request_ || !send_ || disconnected_ ||
        control.last.sequence == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    auto request = control.last;
    request.request = next_request_++;
    request.sequence++;
    request.phase = phase;
    request.position = position;
    request.intent = phase == LayoutControlPhase::End ? control.intent : LayoutControlIntent::None;
    try {
        ValidateLayoutControl(request);
        if (!send_(request)) {
            return false;
        }
    } catch (...) {
        return false;
    }

    control.last = request;
    control.waiting = true;
    return true;
}

void LayoutControlBridge::Pump(std::uint64_t gesture)
{
    const auto it = controls_.find(gesture);
    if (it == controls_.end() || it->second.waiting || !it->second.last.session) {
        return;
    }
    auto &control = it->second;
    if (control.update) {
        const auto event = std::exchange(control.update, std::nullopt);
        if (!Submit(control, LayoutControlPhase::Update, event->position)) {
            Failed(gesture);
        }
    } else if (control.terminal) {
        const auto &event = *control.terminal;
        const auto phase = event.phase == GesturePhase::Cancel ? LayoutControlPhase::Cancel
                                                               : LayoutControlPhase::End;
        if (!Submit(control, phase, event.position)) {
            Failed(gesture);
        }
    }
}

void LayoutControlBridge::Receive(const LayoutControlResult &result)
{
    const auto it = controls_.find(result.gesture);
    if (it == controls_.end()) {
        return;
    }
    auto &control = it->second;
    const bool cancellation = result.status == LayoutControlStatus::Cancelled && result.session &&
                              result.session == control.last.session &&
                              result.sequence <= control.last.sequence;
    if (!cancellation && (!control.waiting || result.request != control.last.request ||
                          result.sequence != control.last.sequence ||
                          (control.last.session && result.session != control.last.session))) {
        return;
    }
    if (result.status == LayoutControlStatus::Began &&
        (control.last.phase != LayoutControlPhase::Begin || !result.session)) {
        return;
    }
    if (result.status == LayoutControlStatus::Updated &&
        control.last.phase != LayoutControlPhase::Update) {
        return;
    }
    if (result.status == LayoutControlStatus::Ended &&
        control.last.phase != LayoutControlPhase::End) {
        return;
    }

    results_.push_back(result);
    if (result.status == LayoutControlStatus::Rejected ||
        result.status == LayoutControlStatus::Cancelled ||
        result.status == LayoutControlStatus::Ended) {
        controls_.erase(it);
        return;
    }
    control.last.session = result.session;
    control.waiting = false;
    Pump(result.gesture);
}

void LayoutControlBridge::Failed(std::uint64_t gesture)
{
    const auto it = controls_.find(gesture);
    if (it == controls_.end()) {
        return;
    }
    const auto &request = it->second.last;
    LayoutControlResult result;
    result.request = request.request;
    result.gesture = gesture;
    result.session = request.session;
    result.sequence = request.sequence;
    result.status = LayoutControlStatus::Cancelled;
    result.error = LayoutControlError::Disconnected;
    result.position = it->second.latest.position;
    results_.push_back(result);
    controls_.erase(it);
}

void LayoutControlBridge::Disconnect()
{
    disconnected_ = true;
    observed_.reset();
    while (!controls_.empty()) {
        Failed(controls_.begin()->first);
    }
}

std::vector<LayoutControlResult> LayoutControlBridge::TakeResults()
{
    return std::exchange(results_, {});
}

std::size_t LayoutControlBridge::ActiveCount() const
{
    return controls_.size();
}

} // namespace prism::sdk
