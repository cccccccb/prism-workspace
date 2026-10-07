#include "scene_p.hpp"

#include <limits>
#include <stdexcept>

namespace prism::runtime {
void Scene::StartSlider(Node &node, contracts::InputSource source, std::uint32_t key,
                        contracts::LogicalRect track)
{
    if (input_state_->sliders.size() >= 256 ||
        input_state_->slider_sequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("Slider interaction limit exceeded");
    }
    InputState::SliderStream stream;
    stream.node = node.id;
    stream.action = node.action;
    stream.source = source;
    stream.key = key;
    stream.track = track;
    stream.domain = node.number_domain;
    stream.revision = node.control_revision;
    stream.interaction = ++input_state_->slider_sequence;
    // Business values are accepted anywhere inside the domain. Quantize user proposals,
    // not the authoritative before value (which may intentionally be off-grid).
    stream.session = ControlValueSession(
        NumberDomain{stream.domain.minimum, stream.domain.maximum, 0}, node.value, stream.revision);
    stream.session.Begin();
    input_state_->sliders.reserve(input_state_->sliders.size() + 1);
    TrackInputTarget(node.id);

    for (std::size_t i = 0; i < input_state_->sliders.size(); ++i) {
        const auto &existing = input_state_->sliders[i];
        if (existing.node == node.id || existing.source == source) {
            FinishSlider(i, ValueCancelReason::Superseded);
        }
    }
    CancelControlCapture(node.id);
    SetInputFocus(node.id, source.seat, key != 0);
    input_state_->sliders.push_back(std::move(stream));
}

void Scene::PreviewSlider(std::size_t index, double value)
{
    auto &stream = input_state_->sliders[index];
    if (stream.terminal || !std::isfinite(value)) {
        return;
    }
    const ControlValueSession normalized(stream.domain, value);
    if (!stream.session.Preview(1, normalized.AuthoritativeValue())) {
        return;
    }
    stream.preview_pending = true;
    if (auto *node = Find(stream.node)) {
        ++node->revision;
    }
    Invalidate(Dirty::Paint);
}

void Scene::FinishSlider(std::size_t index, ValueCancelReason reason) noexcept
{
    auto &stream = input_state_->sliders[index];
    if (stream.terminal) {
        return;
    }
    if (reason == ValueCancelReason::None) {
        stream.terminal = stream.session.Commit(1, stream.session.PresentedValue());
    } else {
        stream.terminal = stream.session.Cancel(1, reason);
    }
    stream.preview_pending = false;
    if (auto *node = Find(stream.node)) {
        ++node->revision;
    }
    Invalidate(Dirty::Paint);
}

void Scene::ReconcileSliders() noexcept
{
    for (std::size_t i = 0; i < input_state_->sliders.size(); ++i) {
        const auto &stream = input_state_->sliders[i];
        if (stream.terminal) {
            continue;
        }
        const auto *node = Find(stream.node);
        if (!node || !IsInteractive(stream.node) || node->action != stream.action) {
            FinishSlider(i, ValueCancelReason::Unavailable);
        } else if (node->control_revision != stream.revision) {
            FinishSlider(i, ValueCancelReason::Superseded);
        }
    }
}

void Scene::ReconcileSliderGeometry(const InputSnapshot &snapshot) noexcept
{
    for (std::size_t i = 0; i < input_state_->sliders.size(); ++i) {
        const auto &stream = input_state_->sliders[i];
        if (HasPopupSurfaceAdoption() && !IsPopupInputSnapshot(&snapshot) &&
            DescendantOf(Find(stream.node), *Find(active_popup_))) {
            continue;
        }
        const auto *item = snapshot.Find(stream.node);
        if (!stream.terminal && (!item || !IsInteractive(stream.node, &snapshot) ||
                                 (!stream.key && item->slider_track != stream.track))) {
            FinishSlider(i, ValueCancelReason::Unavailable);
        }
    }
}

std::vector<ControlEdit> Scene::TakeControlEvents()
{
    std::vector<ControlEdit> result;
    result.reserve(input_state_->sliders.size());
    for (const auto &stream : input_state_->sliders) {
        if (stream.terminal || stream.preview_pending) {
            auto event = stream.terminal.value_or(ControlValueEvent{
                1, stream.revision, ValuePhase::Preview, stream.session.AuthoritativeValue(),
                stream.session.PresentedValue(), ValueCancelReason::None});
            event.interaction = stream.interaction;
            result.push_back({stream.node, stream.action, std::move(event)});
        }
    }
    // Cancellation and transaction reconciliation only mark retained streams; allocation
    // is confined to this drain and input preparation, not noexcept commit paths.
    for (auto &stream : input_state_->sliders) {
        stream.preview_pending = false;
    }
    std::erase_if(input_state_->sliders, [](const InputState::SliderStream &stream) {
        return stream.terminal.has_value();
    });
    return result;
}
} // namespace prism::runtime
