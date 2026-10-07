#include "scene_p.hpp"

namespace prism::runtime {
namespace {
bool SliderKey(std::uint32_t key)
{
    return key == 0x4a || key == 0x4b || key == 0x4d || key == 0x4e || (key >= 0x4f && key <= 0x52);
}

double KeyProposal(std::uint32_t key, double current, const NumberDomain &domain)
{
    if (key == 0x4a) {
        return domain.minimum;
    }
    if (key == 0x4d) {
        return domain.maximum;
    }
    const double step = domain.step > 0 ? domain.step : (domain.maximum - domain.minimum) / 100;
    const double direction = key == 0x50 || key == 0x51 || key == 0x4e ? -1 : 1;
    return current + direction * step * (key == 0x4b || key == 0x4e ? 10 : 1);
}

double PointerProposal(double x, contracts::LogicalRect track, const NumberDomain &domain)
{
    const double fraction = std::clamp((x - track.x) / track.width, 0.0, 1.0);
    return domain.minimum + fraction * (domain.maximum - domain.minimum);
}
} // namespace

bool Scene::HandleSliderInput(const contracts::WindowEvent &event,
                              const std::shared_ptr<const InputSnapshot> &snapshot, bool submitted)
{
    ReconcileSliders();
    auto &streams = input_state_->sliders;
    if (const auto *key = std::get_if<contracts::KeyEvent>(&event)) {
        if (key->state == contracts::ButtonState::Pressed && key->physical_key == 0x29) {
            for (std::size_t i = 0; i < streams.size(); ++i) {
                if (streams[i].source.seat == key->source.seat) {
                    FinishSlider(i, ValueCancelReason::Escape);
                }
            }
            return false; // Also cancel ordinary input captures on this seat.
        }
        std::size_t active = streams.size();
        for (std::size_t i = 0; i < streams.size(); ++i) {
            if (!streams[i].terminal && streams[i].key && streams[i].source == key->source) {
                active = i;
                break;
            }
        }
        if (key->modifiers.control || key->modifiers.alt || key->modifiers.meta) {
            if (active < streams.size()) {
                FinishSlider(active, ValueCancelReason::FocusLost);
            }
            return false;
        }
        if (!SliderKey(key->physical_key)) {
            return false;
        }
        if (key->state == contracts::ButtonState::Released) {
            if (active < streams.size() && streams[active].key == key->physical_key) {
                FinishSlider(active, ValueCancelReason::None);
                return true;
            }
            return false;
        }
        Node *node = nullptr;
        for (const auto &focus : input_state_->focus) {
            if (focus.seat == key->source.seat) {
                node = Find(focus.node);
                break;
            }
        }
        if (!node || node->kind != Kind::Slider || !IsInteractive(node->id) ||
            (submitted && !IsInteractive(node->id, snapshot.get()))) {
            return false;
        }
        if (active == streams.size() || streams[active].key != key->physical_key) {
            if (key->repeat) {
                return true; // A repeat cannot revive an escaped or superseded stream.
            }
            StartSlider(*node, key->source, key->physical_key, {});
            active = streams.size() - 1;
        } else if (!key->repeat) {
            return true;
        }
        const auto &stream = streams[active];
        PreviewSlider(active, KeyProposal(key->physical_key,
                                          std::get<double>(stream.session.PresentedValue()),
                                          stream.domain));
        return true;
    }

    if (const auto *cancel = std::get_if<contracts::PointerCancelEvent>(&event)) {
        for (std::size_t i = 0; i < streams.size(); ++i) {
            if (!streams[i].key && streams[i].source == cancel->source) {
                FinishSlider(i, ValueCancelReason::Unavailable);
            }
        }
        return false;
    }
    const auto *button = std::get_if<contracts::PointerButtonEvent>(&event);
    const auto *motion = std::get_if<contracts::PointerMotionEvent>(&event);
    if ((!button && !motion) || (button && button->button != contracts::PointerButton::Primary)) {
        return false;
    }
    const auto source = button ? button->source : motion->source;
    const auto position = button ? button->position : motion->position;
    if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
        return true;
    }
    std::size_t active = streams.size();
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (!streams[i].terminal && !streams[i].key && streams[i].source == source) {
            active = i;
            break;
        }
    }
    if (active < streams.size()) {
        if (button && button->state == contracts::ButtonState::Pressed) {
            return true;
        }
        const bool inside =
            std::any_of(input_state_->pointers.begin(), input_state_->pointers.end(),
                        [source](const InputState::Pointer &pointer) {
                            return pointer.source == source && pointer.inside;
                        });
        if (motion || inside) {
            MoveInputPointer(source, position, snapshot, submitted);
            PreviewSlider(
                active, PointerProposal(position.x, streams[active].track, streams[active].domain));
        }
        if (button) {
            FinishSlider(active, ValueCancelReason::None);
        }
        return true;
    }
    if (!button || button->state != contracts::ButtonState::Pressed) {
        return false;
    }
    const auto hit = InputHit(position, snapshot, submitted);
    auto *node = hit ? Find(hit->node) : nullptr;
    if (!node || node->kind != Kind::Slider || !IsInteractive(node->id)) {
        return false;
    }
    const auto *geometry = snapshot ? snapshot->Find(node->id) : nullptr;
    const auto track = submitted ? geometry->slider_track : SliderTrack(*node);
    if (track.width <= 0 || track.height <= 0) {
        return true;
    }
    MoveInputPointer(source, position, snapshot, submitted);
    StartSlider(*node, source, 0, track);
    PreviewSlider(streams.size() - 1, PointerProposal(position.x, track, node->number_domain));
    return true;
}
} // namespace prism::runtime
