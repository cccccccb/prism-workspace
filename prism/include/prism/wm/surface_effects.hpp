#pragma once
#include <memory>
#include <span>
#include "prism/contracts/theme.hpp"
struct wl_display;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_scene;
namespace prism::wm {
struct WlrXdgView;
// Compositor-only typed backdrop/material pass. Never sees client DSL or Skia.
class SurfaceEffects {
public:
    SurfaceEffects(wl_display*,wlr_renderer*,wlr_allocator*);
    ~SurfaceEffects();
    bool Supported() const;
    void Update(wlr_scene*,std::span<WlrXdgView* const>,WlrXdgView* focused,
                const contracts::ThemeSnapshot* theme);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
