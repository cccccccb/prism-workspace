#include "prism/runtime/scene_snapshot.hpp"
#include "scene_p.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace prism::runtime {
namespace {
constexpr std::uint64_t kFallbackSampleNs = 8'000'000;
constexpr std::uint64_t kMinimumWakeNs = 1'000'000;

const animation::SystemAnimationClock &DefaultClock()
{
    static const animation::SystemAnimationClock clock;
    return clock;
}

double SrgbToLinear(std::uint8_t channel)
{
    const double value = static_cast<double>(channel) / 255.0;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

std::uint8_t LinearToSrgb(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    const double encoded =
        value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0));
}

contracts::Color BlendColor(contracts::Color from, contracts::Color target, double progress)
{
    const double from_alpha = static_cast<double>(from.a) / 255.0;
    const double target_alpha = static_cast<double>(target.a) / 255.0;
    const double alpha = std::lerp(from_alpha, target_alpha, progress);
    const auto component = [from_alpha, target_alpha, alpha, progress](std::uint8_t start,
                                                                       std::uint8_t end) {
        if (alpha <= 0.0) {
            return std::uint8_t{};
        }
        const double premultiplied =
            std::lerp(SrgbToLinear(start) * from_alpha, SrgbToLinear(end) * target_alpha, progress);
        return LinearToSrgb(premultiplied / alpha);
    };

    return {component(from.r, target.r), component(from.g, target.g), component(from.b, target.b),
            static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0))};
}

animation::DurationSpec Duration(const TransitionSpec &spec)
{
    return {static_cast<std::uint64_t>(spec.duration_ms) * 1'000'000, 0, spec.easing};
}

const TransitionSpec *FindTransition(const std::vector<TransitionSpec> &transitions,
                                     DslProperty property)
{
    const auto found = std::find_if(
        transitions.begin(), transitions.end(),
        [property](const TransitionSpec &transition) { return transition.property == property; });
    return found == transitions.end() ? nullptr : &*found;
}

std::uint64_t AddSaturated(std::uint64_t start, std::uint64_t offset)
{
    return start > std::numeric_limits<std::uint64_t>::max() - offset
               ? std::numeric_limits<std::uint64_t>::max()
               : start + offset;
}
} // namespace

PropertyValue Scene::AnimationState::Track::Interpolate(double progress_value) const
{
    if (progress_value <= 0.0) {
        return from;
    }
    if (progress_value >= 1.0) {
        return target;
    }

    const auto *spec = FindProperty(property);
    if (spec && spec->stored_type == StoredValueType::Number) {
        return std::lerp(std::get<double>(from), std::get<double>(target), progress_value);
    }
    if (spec && spec->stored_type == StoredValueType::Color) {
        return BlendColor(std::get<contracts::Color>(from), std::get<contracts::Color>(target),
                          progress_value);
    }
    return target;
}

void Scene::EnableAnimations(const animation::AnimationClock *clock)
{
    const auto &selected = clock ? *clock : DefaultClock();
    if (animation_state_) {
        if (&animation_state_->clock != &selected) {
            throw std::logic_error("Cannot change an active Scene animation clock");
        }
        return;
    }

    animation_state_ = std::make_unique<AnimationState>(selected);
}

bool Scene::HasActiveAnimations() const noexcept
{
    return animation_state_ && !animation_state_->tracks.empty();
}

std::uint64_t Scene::AnimationNowNs() const noexcept
{
    return animation_state_ ? animation_state_->clock.NowNs() : DefaultClock().NowNs();
}

void Scene::RecordAnimationSample(std::uint64_t now) noexcept
{
    if (animation_sample_.revision != std::numeric_limits<std::uint64_t>::max()) {
        ++animation_sample_.revision;
    }
    animation_sample_.time_ns = now;
}

bool Scene::RetargetPresentation(Node &node, DslProperty property, const PropertyValue &previous,
                                 const PropertyValue &target, std::uint64_t now)
{
    if (!animation_state_ || !IsVisible(node)) {
        return false;
    }

    const auto *spec = FindTransition(node.transitions, property);
    if (!spec || !spec->duration_ms) {
        return false;
    }

    const AnimationState::Key key{node.id.index, property};
    auto found = animation_state_->tracks.find(key);
    if (found != animation_state_->tracks.end() && found->second->node != node.id) {
        animation_state_->tracks.erase(found);
        found = animation_state_->tracks.end();
    }
    PropertyValue visual = previous;
    if (found != animation_state_->tracks.end()) {
        auto &old = *found->second;
        visual = old.Interpolate(old.progress.SampleAt(now).value);
        if (old.presented != visual) {
            old.presented = visual;
            ++node.revision;
            Invalidate(Dirty::Paint);
            RecordAnimationSample(now);
        }
    }
    if (visual == target) {
        if (found != animation_state_->tracks.end()) {
            animation_state_->tracks.erase(found);
        }
        return false;
    }

    if (found == animation_state_->tracks.end()) {
        auto track = std::make_unique<AnimationState::Track>(animation_state_->clock, node.id,
                                                             property, visual, target);
        if (!track->progress.StartDurationAt(0.0, 1.0, Duration(*spec), now)) {
            return false;
        }
        animation_state_->tracks.emplace(key, std::move(track));
    } else {
        auto &track = *found->second;
        track.from = visual;
        track.target = target;
        track.presented = visual;
        track.progress.RestartDurationAt(0.0, 1.0, Duration(*spec), now);
    }
    return true;
}

bool Scene::AdvanceAnimations(std::uint64_t now)
{
    if (!animation_state_) {
        return false;
    }

    bool changed = false;
    for (auto it = animation_state_->tracks.begin(); it != animation_state_->tracks.end();) {
        auto &track = *it->second;
        Node *node = Find(track.node);
        if (!node || !IsVisible(*node)) {
            it = animation_state_->tracks.erase(it);
            continue;
        }

        const auto sample = track.progress.SampleAt(now);
        const auto visual = track.Interpolate(sample.value);
        if (track.presented != visual) {
            track.presented = visual;
            ++node->revision;
            Invalidate(Dirty::Paint);
            changed = true;
        }
        if (sample.state == animation::MotionState::Finished ||
            sample.state == animation::MotionState::Cancelled) {
            it = animation_state_->tracks.erase(it);
        } else {
            ++it;
        }
    }
    if (changed) {
        RecordAnimationSample(now);
    }
    return changed;
}

std::optional<std::uint64_t> Scene::NextAnimationDeadlineNs(std::uint64_t now) const noexcept
{
    if (!HasActiveAnimations()) {
        return std::nullopt;
    }

    std::uint64_t next = std::numeric_limits<std::uint64_t>::max();
    for (const auto &[key, owned] : animation_state_->tracks) {
        (void)key;
        const auto end = owned->progress.CompletionDeadlineNs();
        const auto fallback = AddSaturated(now, kFallbackSampleNs);
        const auto candidate = end ? std::min(*end, fallback) : fallback;
        next = std::min(next, candidate);
    }
    return next == std::numeric_limits<std::uint64_t>::max()
               ? std::optional<std::uint64_t>(AddSaturated(now, kMinimumWakeNs))
               : std::optional<std::uint64_t>(next);
}

void Scene::ApplyPresentation(const Node &node, SnapshotNode &snapshot) const
{
    if (!animation_state_) {
        return;
    }

    for (const auto &spec : node.transitions) {
        const auto found = animation_state_->tracks.find({node.id.index, spec.property});
        if (found == animation_state_->tracks.end() || found->second->node != node.id) {
            continue;
        }

        const auto &value = found->second->presented;
        if (spec.property == DslProperty::Value) {
            snapshot.value = std::get<double>(value);
        } else if (spec.property == DslProperty::Foreground) {
            snapshot.style.foreground = std::get<contracts::Color>(value);
        }
    }
}

void Scene::CancelAnimations() noexcept
{
    if (!animation_state_) {
        return;
    }

    bool changed = false;
    for (const auto &[key, owned] : animation_state_->tracks) {
        (void)key;
        Node *node = Find(owned->node);
        if (node && IsVisible(*node) &&
            owned->presented != CurrentProperty(*node, owned->property)) {
            ++node->revision;
            Invalidate(Dirty::Paint);
            changed = true;
        }
    }
    if (changed) {
        RecordAnimationSample(animation_state_->clock.NowNs());
    }
    animation_state_->tracks.clear();
}

void Scene::CancelHiddenAnimations() noexcept
{
    if (!animation_state_) {
        return;
    }

    for (auto it = animation_state_->tracks.begin(); it != animation_state_->tracks.end();) {
        Node *node = Find(it->second->node);
        if (!node || !IsVisible(*node)) {
            it = animation_state_->tracks.erase(it);
        } else {
            ++it;
        }
    }
}

void Scene::ReconcileCommittedAnimations(
    const std::vector<std::pair<Node *, Node *>> &pairs) noexcept
{
    if (!animation_state_) {
        return;
    }

    const auto now = animation_state_->clock.NowNs();
    for (const auto &[live, previous] : pairs) {
        for (const auto &spec : live->transitions) {
            const auto old_value = CurrentProperty(*previous, spec.property);
            const auto target = CurrentProperty(*live, spec.property);
            if (old_value == target) {
                continue;
            }

            try {
                (void)RetargetPresentation(*live, spec.property, old_value, target, now);
            } catch (const std::bad_alloc &) {
                animation_state_->tracks.erase({live->id.index, spec.property});
            }
        }
    }
}

void Scene::DropAnimationsForNodes(const std::vector<Node *> &nodes) noexcept
{
    if (!animation_state_) {
        return;
    }

    for (const Node *node : nodes) {
        for (const auto &spec : node->transitions) {
            const AnimationState::Key key{node->id.index, spec.property};
            const auto found = animation_state_->tracks.find(key);
            if (found != animation_state_->tracks.end() && found->second->node == node->id) {
                animation_state_->tracks.erase(found);
            }
        }
    }
}
} // namespace prism::runtime
