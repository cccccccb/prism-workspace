#include "surface_effects_internal.hpp"

namespace prism::wm {
namespace {
struct PaintGeometry {
    int x{}, y{}, width{}, height{};
    double local_x{}, local_y{};
};

std::optional<PaintGeometry> Geometry(const Region &region, contracts::LogicalRect view,
                                      double decoration_extent)
{
    const double padding = std::ceil(std::max(region.blur_radius, decoration_extent));
    const double left = view.x + region.bounds.x;
    const double top = view.y + region.bounds.y;
    const double right = left + region.bounds.width;
    const double bottom = top + region.bounds.height;
    if (!std::isfinite(padding) || !std::isfinite(left) || !std::isfinite(top) ||
        !std::isfinite(right) || !std::isfinite(bottom) || padding < 0 ||
        region.bounds.width <= 0 || region.bounds.height <= 0) {
        return std::nullopt;
    }

    const double x = std::floor(left) - padding;
    const double y = std::floor(top) - padding;
    const double width = std::ceil(right) - x + padding;
    const double height = std::ceil(bottom) - y + padding;
    constexpr double origin_limit = 1e8;
    if (std::abs(x) > origin_limit || std::abs(y) > origin_limit || width <= 0 || height <= 0 ||
        width > 8192 || height > 8192) {
        return std::nullopt;
    }
    return PaintGeometry{int(x), int(y), int(width), int(height), left - x, top - y};
}

bool RejectEffectInput(wlr_scene_buffer *, double *, double *)
{
    return false;
}

struct EffectTarget {
    wlr_surface *surface{};
    wlr_scene_tree *tree{};
    WlrXdgView *view{};
    contracts::LogicalRect bounds{};
    int parent_x{}, parent_y{};
};

struct SurfaceOrigin {
    wlr_surface *surface{};
    std::optional<contracts::LogicalPoint> point;

    static void Find(wlr_scene_buffer *buffer, int, int, void *data)
    {
        auto &search = *static_cast<SurfaceOrigin *>(data);
        const auto *leaf = wlr_scene_surface_try_from_buffer(buffer);
        int x{};
        int y{};
        if (leaf && leaf->surface == search.surface &&
            wlr_scene_node_coords(&buffer->node, &x, &y)) {
            search.point = contracts::LogicalPoint{double(x), double(y)};
        }
    }
};

std::optional<EffectTarget> PreparePopupTarget(const SurfaceEffects::Target &target)
{
    if (!target.surface || !target.tree || !target.tree->node.parent) {
        return std::nullopt;
    }

    SurfaceOrigin search{target.surface, {}};
    wlr_scene_node_for_each_buffer(&target.tree->node, SurfaceOrigin::Find, &search);
    EffectTarget result{target.surface, target.tree};
    if (!search.point || !wlr_scene_node_coords(&target.tree->node.parent->node, &result.parent_x,
                                                &result.parent_y)) {
        return std::nullopt;
    }
    result.bounds = {search.point->x, search.point->y, double(target.surface->current.width),
                     double(target.surface->current.height)};
    return result;
}

void CollectOrderedTargets(wlr_scene_node *node,
                           const std::map<wlr_scene_node *, EffectTarget> &by_node,
                           std::vector<const EffectTarget *> &ordered)
{
    if (auto it = by_node.find(node); it != by_node.end()) {
        ordered.push_back(&it->second);
    }
    if (node->type != WLR_SCENE_NODE_TREE) {
        return;
    }
    auto *tree = wlr_scene_tree_from_node(node);
    wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link)
    {
        CollectOrderedTargets(child, by_node, ordered);
    }
}
} // namespace

SurfaceEffects::UpdateResult SurfaceEffects::Impl::Update(wlr_scene *scene,
                                                          std::span<WlrXdgView *const> views,
                                                          WlrXdgView *focused,
                                                          const contracts::ThemeSnapshot *theme,
                                                          std::span<const Target> popups)
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
    std::map<wlr_scene_node *, EffectTarget> by_node;
    for (auto *view : views) {
        if (view->mapped && view->visible) {
            alive.insert(view->toplevel->base->surface);
            by_node.emplace(&view->scene_tree->node,
                            EffectTarget{view->toplevel->base->surface, view->scene_tree, view});
        }
    }

    for (const auto &popup : popups) {
        if (auto target = PreparePopupTarget(popup)) {
            alive.insert(target->surface);
            by_node.emplace(&target->tree->node, *target);
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
    std::vector<const EffectTarget *> ordered;
    CollectOrderedTargets(&scene->tree.node, by_node, ordered);
    // Resolve and paint in actual lower-to-upper scene order. A reused GPU
    // buffer carries a content generation, not just its stable pointer.
    for (const auto *target : ordered) {
        auto *view = target->view;
        auto *surface = target->surface;
        const auto state =
            ResolveDecoration(theme, view && view == focused, view && view->fullscreen,
                              !view || view->shell_role != 0);
        auto style = view && view->presentation ? view->presentation->decoration : state.style;
        if (!view) {
            // Popup content, border and shadow are painted by its own DSL target.
            style.enabled = false;
        }
        const auto bounds = !view ? target->bounds
                            : view->presentation
                                ? view->presentation->bounds
                                : contracts::LogicalRect{double(view->x), double(view->y),
                                                         double(view->width), double(view->height)};
        std::vector<Region> regions;
        if (auto it = states.find(surface); it != states.end()) {
            regions = it->second->current;
        }
        if (view && view->presentation) {
            const auto &source = view->presentation->source;
            const double scale_x = bounds.width / source.width,
                         scale_y = bounds.height / source.height;
            for (auto &region : regions) {
                if (region.contour) {
                    // The transported polygon remains canonical in State. This
                    // copy is mapped once into presentation space without a
                    // second quantization or independent curve preparation.
                    for (auto &point : region.contour->points) {
                        point.x = (point.x - source.x) * scale_x;
                        point.y = (point.y - source.y) * scale_y;
                    }
                    region.bounds = contracts::ContourBounds(*region.contour);
                } else {
                    region.bounds = {(region.bounds.x - source.x) * scale_x,
                                     (region.bounds.y - source.y) * scale_y,
                                     region.bounds.width * scale_x, region.bounds.height * scale_y};
                }
                region.corner_radius *= std::min(scale_x, scale_y);
            }
        }
        auto is_frame = [&](const Region &r) {
            return !r.contour && style.enabled && std::abs(r.bounds.x) < .01 &&
                   std::abs(r.bounds.y) < .01 && std::abs(r.bounds.width - bounds.width) < .01 &&
                   std::abs(r.bounds.height - bounds.height) < .01;
        };
        if (style.enabled && std::none_of(regions.begin(), regions.end(), is_frame)) {
            regions.insert(regions.begin(), {{0, 0, bounds.width, bounds.height}, style.radius, 0});
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
                paint->tree = wlr_scene_tree_create(target->tree->node.parent);
                if (!paint->tree) {
                    throw std::bad_alloc();
                }
                wl_signal_add(&paint->tree->node.events.destroy, &paint->tree_destroy.listener);
                paint->node = wlr_scene_buffer_create(paint->tree, nullptr);
                if (!paint->node) {
                    throw std::bad_alloc();
                }
                paint->node->point_accepts_input = RejectEffectInput;
                result.scene_changed = true;
            }
        }

        // Anchor from the client backwards. Moving a successor later in
        // forward order could separate earlier paints from their client.
        auto *successor = &target->tree->node;
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
            const auto geometry = Geometry(r, bounds, decoration_extent);
            auto fail = [&]() {
                ++counters.failed_regions;
                p.valid = false;
                if (p.tree && p.tree->node.enabled) {
                    wlr_scene_node_set_enabled(&p.tree->node, false);
                    result.scene_changed = true;
                }
            };
            if (!geometry) {
                ++counters.invalid_regions;
                fail();
                continue;
            }
            if (!p.tree->node.enabled) {
                wlr_scene_node_set_enabled(&p.tree->node, true);
                result.scene_changed = true;
            }
            const int x = geometry->x;
            const int y = geometry->y;
            const int w = geometry->width;
            const int h = geometry->height;
            const int parent_x = target->parent_x;
            const int parent_y = target->parent_y;
            if (p.tree->node.x != x - parent_x || p.tree->node.y != y - parent_y) {
                wlr_scene_node_set_position(&p.tree->node, x - parent_x, y - parent_y);
                result.scene_changed = true;
            }

            std::set<wlr_scene_node *> own;
            for (const auto &paint : list) {
                if (paint->tree) {
                    own.insert(&paint->tree->node);
                }
            }
            Walk walk{&target->tree->node, &p.tree->node, renderer};
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
            walk.Value(bool(r.contour));
            if (r.contour) {
                walk.Value(r.contour->points.size());
                for (auto point : r.contour->points) {
                    walk.Value(point.x);
                    walk.Value(point.y);
                }
            }
            walk.Value(geometry->local_x);
            walk.Value(geometry->local_y);
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
            if (!PrepareMask(p, r, w, h, geometry->local_x, geometry->local_y)) {
                fail();
                continue;
            }
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
                Walk capture{&target->tree->node, &p.tree->node, renderer, pass,
                             double(x),           double(y),     .5};
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

            if (!Draw(*p.result, *p.source, material, w, h, &r, geometry->local_x,
                      geometry->local_y, decoration ? &style : nullptr, &p)) {
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
                               r.blur_radius, view ? view->shell_role : -1);
            }
        }
    }
    return result;
}

} // namespace prism::wm
