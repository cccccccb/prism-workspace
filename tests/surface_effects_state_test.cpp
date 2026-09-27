#include "prism/wm/surface_effects.hpp"
#include <cstdio>
#include <memory>
#include <wayland-server-core.h>
extern "C" {
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
}

int main()
{
    auto *display = wl_display_create();
    auto *renderer = wlr_pixman_renderer_create();
    if (!display || !renderer) {
        return 1;
    }
    int wakes = 0;
    auto effects = std::make_unique<prism::wm::SurfaceEffects>(display, renderer, nullptr);
    auto check = [](bool condition, const char *message) {
        if (!condition) {
            std::fprintf(stderr, "surface effects state: %s\n", message);
        }
        return condition;
    };
    effects->SetWakeHandler([&] { ++wakes; });
    if (!check(!effects->Supported() && effects->NeedsUpdate() && wakes == 1,
               "initial work must wake even when installed after construction")) {
        return 2;
    }
    effects->MarkDirty();
    effects->MarkDirty();
    if (!check(wakes == 1, "pending invalidations must coalesce")) {
        return 3;
    }
    auto first = effects->Update(nullptr, {}, nullptr, nullptr);
    if (!check(!first.processed && !first.scene_changed && !effects->NeedsUpdate(),
               "unsupported renderer must consume pending work without rendering")) {
        return 4;
    }
    effects->Update(nullptr, {}, nullptr, nullptr);
    if (!check(effects->Counters().update_calls == 2 &&
                   effects->Counters().unsupported_updates == 1 &&
                   effects->Counters().skipped_updates == 1,
               "unsupported and unchanged calls must have distinct counters")) {
        return 5;
    }
    effects->MarkDirty();
    effects->MarkDirty();
    if (!check(wakes == 2 && effects->Counters().dirty_transitions == 1,
               "new pending work must issue exactly one wake")) {
        return 6;
    }
    effects->Update(nullptr, {}, nullptr, nullptr);
    effects->SetWakeHandler({});
    effects->MarkDirty();
    if (!check(wakes == 2 && effects->NeedsUpdate() && effects->Counters().dirty_transitions == 2 &&
                   effects->Counters().wake_notifications == 2,
               "clearing a handler must retain pending work without calling it")) {
        return 7;
    }
    const auto &counters = effects->Counters();
    if (!check(!counters.regions_checked && !counters.capture_pass_attempts &&
                   !counters.blur_pass_attempts && !counters.material_pass_attempts &&
                   !counters.rendered_regions && !counters.rendered_pixels,
               "unsupported backend must not claim GPU work")) {
        return 8;
    }
    effects.reset();
    wlr_renderer_destroy(renderer);
    wl_display_destroy(display);
    return 0;
}
