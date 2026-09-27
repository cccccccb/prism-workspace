#include "surface_effects_internal.hpp"

namespace prism::wm {
void SurfaceEffects::Impl::NotifyWake()
{
    if (wake_handler) {
        ++counters.wake_notifications;
        wake_handler();
    }
}

void SurfaceEffects::Impl::MarkDirty()
{
    if (!needs_update) {
        needs_update = true;
        ++counters.dirty_transitions;
        NotifyWake();
    }
}

void SurfaceEffects::Impl::NotifySurfaceCommit(wlr_surface *surface)
{
    if (!surface) {
        return;
    }
    auto &content = contents[surface];
    if (!content.identity) {
        content.identity = ++next_surface_identity;
    }
    const auto mapping = SurfaceMapping(surface);
    const bool mapping_changed = !content.initialized || content.mapping != mapping ||
                                 (surface->current.committed & WLR_SURFACE_STATE_OFFSET);
    const bool first_content =
        mapping.has_buffer && (!content.initialized || !content.mapping.has_buffer);
    if (mapping_changed) {
        ++content.mapping_epoch;
    }
    content.mapping = mapping;
    content.initialized = true;
    constexpr auto damage_fields = WLR_SURFACE_STATE_BUFFER | WLR_SURFACE_STATE_SURFACE_DAMAGE |
                                   WLR_SURFACE_STATE_BUFFER_DAMAGE;
    std::vector<LogicalRect> rectangles;
    if (mapping.has_buffer && (surface->current.committed & damage_fields)) {
        pixman_region32_t damage;
        pixman_region32_init(&damage);
        wlr_surface_get_effective_damage(surface, &damage);
        int count{};
        const auto *boxes = pixman_region32_rectangles(&damage, &count);
        // Bounded copied storage; extents conservatively contain every box.
        if (count > int(effects::DamageHistory::kMaxRectangles)) {
            const auto *box = pixman_region32_extents(&damage);
            rectangles.push_back({double(box->x1), double(box->y1), double(box->x2 - box->x1),
                                  double(box->y2 - box->y1)});
        } else {
            for (int i = 0; i < count; ++i) {
                rectangles.push_back({double(boxes[i].x1), double(boxes[i].y1),
                                      double(boxes[i].x2 - boxes[i].x1),
                                      double(boxes[i].y2 - boxes[i].y1)});
            }
        }
        pixman_region32_fini(&damage);
    }
    const bool recorded = content.history.Record(rectangles, content.mapping_epoch, first_content);
    if (recorded) {
        ++counters.content_revisions;
    } else {
        ++counters.metadata_commits;
    }
    if (recorded || mapping_changed) {
        MarkDirty();
    }
}

void SurfaceEffects::Impl::ForgetSurface(wlr_surface *surface)
{
    if (contents.erase(surface)) {
        MarkDirty();
    }
}

} // namespace prism::wm
