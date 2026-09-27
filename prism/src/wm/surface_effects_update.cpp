#include "surface_effects_internal.hpp"

namespace prism::wm {
namespace {
bool RejectEffectInput(wlr_scene_buffer *, double *, double *)
{
    return false;
}

void CollectOrderedViews(wlr_scene_node *node,
                         const std::map<wlr_scene_node *, WlrXdgView *> &by_node,
                         std::vector<WlrXdgView *> &ordered)
{
    if (auto it = by_node.find(node); it != by_node.end()) {
        ordered.push_back(it->second);
    }
    if (node->type != WLR_SCENE_NODE_TREE) {
        return;
    }
    auto *tree = wlr_scene_tree_from_node(node);
    wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link)
    {
        CollectOrderedViews(child, by_node, ordered);
    }
}
} // namespace

SurfaceEffects::UpdateResult SurfaceEffects::Impl::Update(wlr_scene *scene,
                                                          std::span<WlrXdgView *const> views,
                                                          WlrXdgView *focused,
                                                          const contracts::ThemeSnapshot *theme)
{
    ++counters.update_calls;
    if (!needs_update) {
        ++counters.skipped_updates;
        return {};
    }
    needs_update = false;
    if (!supported) {
        ++counters.unsupported_updates;
        return {};
    }

    UpdateResult result{true, false};
    std::set<wlr_surface *> alive;
    std::map<wlr_scene_node *, WlrXdgView *> by_node;
    for (auto *view : views) {
        if (view->mapped && view->visible) {
            alive.insert(view->toplevel->base->surface);
            by_node.emplace(&view->scene_tree->node, view);
        }
    }

    // Retire hidden materials before sampling, otherwise old workspace
    // paints could appear as backdrop siblings of a now hidden client.
    for (auto it = paints.begin(); it != paints.end();) {
        if (!alive.contains(it->first)) {
            counters.removed_regions += it->second.size();
            result.scene_changed |= !it->second.empty();
            it = paints.erase(it);
        } else {
            ++it;
        }
    }

    std::map<wlr_scene_buffer *, std::uint64_t> paint_generations;
    for (const auto &[surface, list] : paints) {
        for (const auto &paint : list) {
            if (paint->node) {
                paint_generations[paint->node] = paint->generation;
            }
        }
    }
    std::vector<WlrXdgView *> ordered;
    CollectOrderedViews(&scene->tree.node, by_node, ordered);
    // Resolve and paint in actual lower-to-upper scene order. A reused GPU
    // buffer carries a content generation, not just its stable pointer.
    for (auto *view : ordered) {
        auto *surface = view->toplevel->base->surface;
        const auto state =
            ResolveDecoration(theme, view == focused, view->fullscreen, view->shell_role != 0);
        const auto &style = state.style;
        std::vector<Region> regions;
        if (auto it = states.find(surface); it != states.end()) {
            regions = it->second->current;
        }
        auto is_frame = [&](const Region &r) {
            return style.enabled && std::abs(r.bounds.x) < .01 && std::abs(r.bounds.y) < .01 &&
                   std::abs(r.bounds.width - view->width) < .01 &&
                   std::abs(r.bounds.height - view->height) < .01;
        };
        if (style.enabled && std::none_of(regions.begin(), regions.end(), is_frame)) {
            regions.insert(regions.begin(),
                           {{0, 0, double(view->width), double(view->height)}, style.radius, 0});
        }
        auto &list = paints[surface];
        while (list.size() > regions.size()) {
            paint_generations.erase(list.back()->node);
            list.pop_back();
            ++counters.removed_regions;
            result.scene_changed = true;
        }
        while (list.size() < regions.size()) {
            list.push_back(std::make_unique<Paint>());
        }
        for (auto &paint : list) {
            if (!paint->tree) {
                paint->tree = wlr_scene_tree_create(view->scene_tree->node.parent);
                paint->node = wlr_scene_buffer_create(paint->tree, nullptr);
                paint->node->point_accepts_input = RejectEffectInput;
                result.scene_changed = true;
            }
        }

        // Anchor from the client backwards. Moving a successor later in
        // forward order could separate earlier paints from their client.
        auto *successor = &view->scene_tree->node;
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            auto *node = &(*it)->tree->node;
            if (node->link.next != &successor->link) {
                wlr_scene_node_place_below(node, successor);
                ++counters.scene_reorders;
                result.scene_changed = true;
            }
            successor = node;
        }

        for (std::size_t i = 0; i < regions.size(); ++i) {
            ++counters.regions_checked;
            auto r = regions[i];
            const bool decoration = is_frame(r);
            if (decoration) {
                r.corner_radius = style.radius;
            }
            auto &p = *list[i];
            const double decoration_extent =
                decoration
                    ? std::max(style.border_width, style.shadow_blur + std::abs(style.shadow_y))
                    : 0;
            const int padding = int(std::ceil(std::max(r.blur_radius, decoration_extent)));
            const int w = int(std::ceil(r.bounds.width)) + 2 * padding,
                      h = int(std::ceil(r.bounds.height)) + 2 * padding;
            auto fail = [&]() {
                ++counters.failed_regions;
                p.valid = false;
                if (p.tree && p.tree->node.enabled) {
                    wlr_scene_node_set_enabled(&p.tree->node, false);
                    result.scene_changed = true;
                }
            };
            if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
                ++counters.invalid_regions;
                fail();
                continue;
            }
            if (!p.tree->node.enabled) {
                wlr_scene_node_set_enabled(&p.tree->node, true);
                result.scene_changed = true;
            }
            const int x = view->x + int(std::floor(r.bounds.x)) - padding,
                      y = view->y + int(std::floor(r.bounds.y)) - padding;
            if (p.tree->node.x != x || p.tree->node.y != y) {
                wlr_scene_node_set_position(&p.tree->node, x, y);
                result.scene_changed = true;
            }

            std::set<wlr_scene_node *> own;
            for (const auto &paint : list) {
                if (paint->tree) {
                    own.insert(&paint->tree->node);
                }
            }
            Walk walk{&view->scene_tree->node, &p.tree->node, renderer};
            walk.excluded = &own;
            walk.paint_generations = &paint_generations;
            walk.footprint = effects::CaptureFootprint(x, y, w, h);
            walk.contents = &contents;
            walk.previous = &p.dependencies;
            walk.counters = &counters;

            if (r.blur_radius > 0) {
                walk.Node(&scene->tree.node);
            }
            walk.Value(r.bounds.x);
            walk.Value(r.bounds.y);
            walk.Value(r.bounds.width);
            walk.Value(r.bounds.height);
            walk.Hash(&r.corner_radius, sizeof(r.corner_radius));
            walk.Hash(&r.blur_radius, sizeof(r.blur_radius));
            walk.Hash(&decoration, sizeof(decoration));
            // Hash individual fields to avoid compiler padding in structs.
            if (decoration) {
                walk.Hash(&style.enabled, sizeof(style.enabled));
                walk.Hash(&style.radius, sizeof(style.radius));
                walk.Hash(&style.border_width, sizeof(style.border_width));
                walk.Hash(&style.border, sizeof(style.border));
                walk.Hash(&style.shadow_blur, sizeof(style.shadow_blur));
                walk.Hash(&style.shadow_y, sizeof(style.shadow_y));
                walk.Hash(&style.shadow, sizeof(style.shadow));
            }
            walk.Hash(&x, sizeof(x));
            walk.Hash(&y, sizeof(y));
            if (p.valid && !walk.dependency_changed && p.key == walk.hash && p.width == w &&
                p.height == h) {
                ++counters.cache_hits;
                if (walk.advanced_without_damage) {
                    ++counters.partial_damage_cache_hits;
                }
                p.dependencies = std::move(walk.candidates);
                continue;
            }

            ++counters.cache_misses;
            if (p.width != w || p.height != h || !p.source || !p.intermediate || !p.result) {
                p.source = Allocate((w + 1) / 2, (h + 1) / 2);
                p.intermediate = Allocate((w + 1) / 2, (h + 1) / 2);
                p.result = Allocate(w, h);
                p.width = w;
                p.height = h;
                if (!p.source || !p.intermediate || !p.result) {
                    PRISM_LOG_ERROR("SURFACE-EFFECT", "Buffer allocation failed");
                    fail();
                    continue;
                }
            }

            if (r.blur_radius > 0) {
                ++counters.capture_pass_attempts;
                auto *pass = wlr_renderer_begin_buffer_pass(renderer, p.source->buffer, nullptr);
                if (!pass) {
                    fail();
                    PRISM_LOG_ERROR("SURFACE-EFFECT", "Capture pass failed");
                    continue;
                }
                wlr_render_rect_options clear{};
                clear.box = {0, 0, p.source->buffer->width, p.source->buffer->height};
                clear.blend_mode = WLR_RENDER_BLEND_MODE_NONE;
                wlr_render_pass_add_rect(pass, &clear);
                Walk capture{&view->scene_tree->node,
                             &p.tree->node,
                             renderer,
                             pass,
                             double(x),
                             double(y),
                             .5};
                capture.excluded = &own;
                capture.footprint = walk.footprint;
                capture.counters = &counters;
                capture.Node(&scene->tree.node);
                const bool submitted = wlr_render_pass_submit(pass);
                const bool captured = submitted && !capture.capture_failed;
                if (captured) {
                    ++counters.capture_passes;
                }
                if (!captured || !Blur(*p.intermediate, *p.source, r.blur_radius / 2, true) ||
                    !Blur(*p.source, *p.intermediate, r.blur_radius / 2, false)) {
                    fail();
                    PRISM_LOG_ERROR("SURFACE-EFFECT", "Capture or blur submission failed");
                    continue;
                }
            }

            if (!Draw(*p.result, *p.source, material, w, h, &r, padding,
                      decoration ? &style : nullptr)) {
                fail();
                PRISM_LOG_ERROR("SURFACE-EFFECT", "Material pass failed");
                continue;
            }

            wlr_scene_buffer_set_buffer(p.node, p.result->buffer);
            p.key = walk.hash;
            p.valid = true;
            p.dependencies = std::move(walk.candidates);
            p.generation = ++generated;
            paint_generations[p.node] = p.generation;
            ++counters.rendered_regions;
            counters.rendered_pixels += std::uint64_t(w) * std::uint64_t(h);
            result.scene_changed = true;
            if (generated <= 12) {
                PRISM_LOG_INFO("SURFACE-EFFECT",
                               "Rendered region %dx%d blur=%.1f lower-scene-only shell=%d", w, h,
                               r.blur_radius, view->shell_role);
            }
        }
    }
    return result;
}

} // namespace prism::wm
