#include "prism/runtime/buffer_damage.hpp"
#include "render_geometry_p.hpp"
#include <variant>
#include <vector>

namespace prism::render_skia::detail {
namespace {
struct DrawState {
    SkMatrix matrix{SkMatrix::I()};
    SkRect clip;
    bool transparent{false};
    bool changed{false};
};

class DamageTraversal {
public:
    DamageTraversal(int width, int height) : state_{SkMatrix::I(), SkRect::MakeWH(width, height)}
    {
    }

    bool Push(const contracts::DrawCommand &command, bool changed)
    {
        stack_.push_back(state_);
        state_.changed = state_.changed || changed;

        if (const auto *transform = std::get_if<contracts::PushTransform>(&command)) {
            state_.matrix = SkMatrix::Concat(state_.matrix, TransformMatrix(*transform));
            // The presentation contract currently exposes positive axis scales
            // and translations. Other affine operations retain full repair.
            return state_.matrix.isFinite() && state_.matrix.isScaleTranslate() &&
                   state_.matrix.getScaleX() > 0 && state_.matrix.getScaleY() > 0;
        }
        if (const auto *opacity = std::get_if<contracts::PushOpacity>(&command)) {
            state_.transparent = state_.transparent || opacity->opacity == 0;
            return true;
        }

        SkRect clip;
        if (const auto *rect = std::get_if<contracts::PushClipRect>(&command)) {
            // Rect-preserving transforms have the same outward device clip in
            // Replay. Arbitrary skewed hard clips retain the full repair gate.
            if (!state_.matrix.rectStaysRect()) {
                return false;
            }
            clip = HardClipBounds(rect->bounds, state_.matrix);
        } else if (const auto *rounded = std::get_if<contracts::PushClipRoundedRect>(&command)) {
            clip = state_.matrix.mapRect(ToSkRect(rounded->bounds));
            clip.outset(2, 2);
        } else {
            const auto &contour = std::get<contracts::PushClipContour>(command);
            clip = state_.matrix.mapRect(ToSkRect(contracts::ContourBounds(contour.contour)));
            clip.outset(2, 2);
        }
        if (!clip.isFinite()) {
            return false;
        }
        if (!state_.clip.intersect(clip)) {
            state_.clip.setEmpty();
        }
        return true;
    }

    void Pop()
    {
        state_ = stack_.back();
        stack_.pop_back();
    }

    bool Changed() const
    {
        return state_.changed;
    }

    bool AddInk(const contracts::DrawCommand &command, const ResourceTable &resources,
                contracts::DamageRegion &damage) const
    {
        if (state_.transparent || state_.clip.isEmpty()) {
            return true;
        }

        SkRect bounds;
        if (!DeviceInkBounds(command, resources, state_.matrix, &bounds)) {
            return false;
        }
        if (bounds.isEmpty()) {
            return true;
        }
        // Device AA and hinted glyphs may touch adjacent pixels. Guard after
        // mapping so small scales retain a full device-space safety margin.
        bounds.outset(2, 2);
        if (!bounds.intersect(state_.clip)) {
            return true;
        }
        SkIRect rounded;
        bounds.roundOut(&rounded);
        damage.rects.push_back({rounded.x(), rounded.y(), rounded.width(), rounded.height()});
        return true;
    }

private:
    DrawState state_;
    std::vector<DrawState> stack_;
};
} // namespace

contracts::DamageRegion CompareDisplayListDamage(const contracts::DisplayList &previous,
                                                 const contracts::DisplayList &next, int width,
                                                 int height, const ResourceTable &resources)
{
    DamageTraversal old_state(width, height);
    DamageTraversal new_state(width, height);
    contracts::DamageRegion damage;

    for (std::size_t i = 0; i < next.commands.size(); ++i) {
        const auto &old = previous.commands[i];
        const auto &current = next.commands[i];
        if (old.index() != current.index()) {
            return contracts::DamageRegion::Full();
        }

        const bool clip = std::holds_alternative<contracts::PushClipRect>(current) ||
                          std::holds_alternative<contracts::PushClipRoundedRect>(current) ||
                          std::holds_alternative<contracts::PushClipContour>(current);
        if (clip && old != current) {
            return contracts::DamageRegion::Full();
        }
        if (clip || std::holds_alternative<contracts::PushTransform>(current) ||
            std::holds_alternative<contracts::PushOpacity>(current)) {
            if (!old_state.Push(old, old != current) || !new_state.Push(current, old != current)) {
                return contracts::DamageRegion::Full();
            }
        } else if (std::holds_alternative<contracts::PopClip>(current) ||
                   std::holds_alternative<contracts::PopTransform>(current) ||
                   std::holds_alternative<contracts::PopOpacity>(current)) {
            old_state.Pop();
            new_state.Pop();
        } else if (old != current || old_state.Changed() || new_state.Changed()) {
            if (!old_state.AddInk(old, resources, damage) ||
                !new_state.AddInk(current, resources, damage)) {
                return contracts::DamageRegion::Full();
            }
        }
    }
    return runtime::NormalizeDamage(
        damage, {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)});
}
} // namespace prism::render_skia::detail
