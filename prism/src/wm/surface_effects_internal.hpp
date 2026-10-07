#pragma once
#include "prism-surface-effects-server.h"
#include "prism/contracts/surface_effect.hpp"
#include "prism/core/logging.hpp"
#include "prism/wm/effect_dependencies.hpp"
#include "prism/wm/surface_effects.hpp"
#include "prism/wm/theme.hpp"
#include "prism/wm/xdg_view.hpp"
extern "C" {
#include <drm_fourcc.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/transform.h>
}
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace prism::wm {
namespace effect_detail {
using Region = contracts::SurfaceEffectRegion;
using LogicalRect = effects::Rect;

struct MappingSignature {
    int width{}, height{}, buffer_width{}, buffer_height{}, scale{};
    wl_output_transform transform{};
    bool has_buffer{}, has_source{}, has_destination{};
    double source_x{}, source_y{}, source_width{}, source_height{};
    int destination_width{}, destination_height{};
    bool operator==(const MappingSignature &) const = default;
};

inline MappingSignature SurfaceMapping(wlr_surface *surface)
{
    const auto &state = surface->current;
    MappingSignature mapping;
    mapping.width = state.width;
    mapping.height = state.height;
    mapping.buffer_width = state.buffer_width;
    mapping.buffer_height = state.buffer_height;
    mapping.scale = state.scale;
    mapping.transform = state.transform;
    mapping.has_buffer = wlr_surface_has_buffer(surface);
    mapping.has_source = state.viewport.has_src;
    mapping.has_destination = state.viewport.has_dst;
    if (mapping.has_source) {
        mapping.source_x = state.viewport.src.x;
        mapping.source_y = state.viewport.src.y;
        mapping.source_width = state.viewport.src.width;
        mapping.source_height = state.viewport.src.height;
    }
    if (mapping.has_destination) {
        mapping.destination_width = state.viewport.dst_width;
        mapping.destination_height = state.viewport.dst_height;
    }
    return mapping;
}

struct SurfaceContent {
    std::uint64_t identity{}, mapping_epoch{};
    MappingSignature mapping;
    effects::DamageHistory history;
    bool initialized{};
};

struct ContextScope {
    EGLDisplay old_display = eglGetCurrentDisplay();
    EGLContext old_context = eglGetCurrentContext();
    EGLSurface old_draw = eglGetCurrentSurface(EGL_DRAW), old_read = eglGetCurrentSurface(EGL_READ);
    EGLDisplay display{};
    bool current{};

    explicit ContextScope(wlr_renderer *renderer)
    {
        auto *egl = wlr_gles2_renderer_get_egl(renderer);
        display = wlr_egl_get_display(egl);
        current = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, wlr_egl_get_context(egl));
    }

    ~ContextScope()
    {
        if (!current) {
            return;
        }
        if (old_display != EGL_NO_DISPLAY) {
            eglMakeCurrent(old_display, old_draw, old_read, old_context);
        } else {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }
};

inline GLuint Shader(GLenum type, const char *source)
{
    auto id = glCreateShader(type);
    glShaderSource(id, 1, &source, nullptr);
    glCompileShader(id);
    GLint success{};
    glGetShaderiv(id, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512]{};
        glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        PRISM_LOG_ERROR("SURFACE-EFFECT", "shader: %s", log);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

inline GLuint Program(const char *fragment)
{
    const char *vertex = "attribute vec2 position; varying vec2 uv; void "
                         "main(){uv=position;gl_Position=vec4(position*2.0-1.0,0.0,1.0);}";
    auto v = Shader(GL_VERTEX_SHADER, vertex), f = Shader(GL_FRAGMENT_SHADER, fragment);
    if (!v || !f) {
        if (v) {
            glDeleteShader(v);
        }
        if (f) {
            glDeleteShader(f);
        }
        return 0;
    }
    auto p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "position");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint success{};
    glGetProgramiv(p, GL_LINK_STATUS, &success);
    if (!success) {
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

constexpr const char *blur_shader = R"(
precision mediump float;varying vec2 uv;uniform sampler2D image;uniform vec2 step_size;
void main(){vec4 c=texture2D(image,uv)*0.227027;
c+=(texture2D(image,uv+step_size*1.384615)+texture2D(image,uv-step_size*1.384615))*0.316216;
c+=(texture2D(image,uv+step_size*3.230769)+texture2D(image,uv-step_size*3.230769))*0.070270;
gl_FragColor=c;})";
constexpr const char *material_shader = R"(
precision highp float;varying vec2 uv;uniform sampler2D image;uniform vec2 size;
uniform sampler2D contour_mask;uniform float contour_enabled;uniform vec2 image_uv_scale;
uniform vec4 box;uniform float radius;uniform float shadow;uniform float blur_enabled;
uniform float border_width;uniform vec4 border_color;uniform vec4 shadow_color;uniform float shadow_offset;
float distance_box(vec2 p){vec2 q=abs(p-(box.xy+box.zw*0.5))-(box.zw*0.5-vec2(radius));return length(max(q,0.0))+min(max(q.x,q.y),0.0)-radius;}
void main(){vec2 logical_uv=uv;vec2 p=logical_uv*size;
float d=distance_box(p);float inside=contour_enabled>0.5?texture2D(contour_mask,logical_uv).a:1.0-smoothstep(-0.5,0.5,d);
vec2 image_uv=uv*image_uv_scale;
vec4 c=texture2D(image,image_uv)*inside*blur_enabled;
if(contour_enabled>0.5){gl_FragColor=c;return;}
float sd=max(distance_box(p-vec2(0.0,shadow_offset)),0.0);
float a=shadow>0.0?exp(-sd*sd/(shadow*shadow*0.32))*shadow_color.a*(1.0-inside):0.0;
c+=vec4(shadow_color.rgb*a,a)*(1.0-c.a);
float ring=(1.0-smoothstep(border_width-0.5,border_width+0.5,abs(d)))*(1.0-inside);
vec4 b=vec4(border_color.rgb*border_color.a,border_color.a)*ring;c=b+c*(1.0-b.a);
gl_FragColor=c;})";

struct Buffer {
    wlr_buffer *buffer{};
    wlr_texture *texture{};

    ~Buffer()
    {
        if (texture) {
            wlr_texture_destroy(texture);
        }
        if (buffer) {
            wlr_buffer_drop(buffer);
        }
    }
};

struct ContourMask {
    wlr_renderer *renderer{};
    GLuint texture{};
    int width{}, height{};
    double origin_x{}, origin_y{};
    contracts::Contour relative;
    ~ContourMask();
};

struct Paint {
    wlr_scene_tree *tree{};
    wlr_scene_buffer *node{};
    std::unique_ptr<Buffer> source, intermediate, result;
    std::unique_ptr<ContourMask> mask;
    int width{}, height{};
    std::uint64_t key{}, generation{};
    bool valid{};
    std::map<wlr_scene_buffer *, effects::DependencyStamp> dependencies;

    struct TreeListener {
        wl_listener listener{};
        Paint *paint{};
    } tree_destroy;

    Paint()
    {
        wl_list_init(&tree_destroy.listener.link);
        tree_destroy.listener.notify = TreeGone;
        tree_destroy.paint = this;
    }

    static void TreeGone(wl_listener *listener, void *)
    {
        auto *paint = reinterpret_cast<TreeListener *>(listener)->paint;
        wl_list_remove(&paint->tree_destroy.listener.link);
        wl_list_init(&paint->tree_destroy.listener.link);
        paint->tree = nullptr;
        paint->node = nullptr;
        paint->valid = false;
    }

    ~Paint()
    {
        wl_list_remove(&tree_destroy.listener.link);
        if (tree) {
            wlr_scene_node_destroy(&tree->node);
        }
    }
};

struct Walk {
    wlr_scene_node *target{};
    wlr_scene_node *skip{};
    wlr_renderer *renderer{};
    wlr_render_pass *pass{};
    double origin_x{}, origin_y{}, scale{1};
    bool stopped{}, dependency_changed{}, advanced_without_damage{}, capture_failed{};
    std::uint64_t hash{1469598103934665603ULL};
    LogicalRect footprint;
    const std::set<wlr_scene_node *> *excluded{};
    const std::map<wlr_scene_buffer *, std::uint64_t> *paint_generations{};
    const std::map<wlr_surface *, SurfaceContent> *contents{};
    const std::map<wlr_scene_buffer *, effects::DependencyStamp> *previous{};
    std::map<wlr_scene_buffer *, effects::DependencyStamp> candidates;
    SurfaceEffects::WorkCounters *counters{};
    std::vector<wlr_texture *> textures;

    Walk(wlr_scene_node *target_node, wlr_scene_node *skip_node, wlr_renderer *r,
         wlr_render_pass *render_pass = nullptr, double x = 0, double y = 0, double factor = 1)
        : target(target_node), skip(skip_node), renderer(r), pass(render_pass), origin_x(x),
          origin_y(y), scale(factor)
    {
    }

    ~Walk()
    {
        for (auto *texture : textures) {
            wlr_texture_destroy(texture);
        }
    }

    void Hash(const void *ptr, std::size_t size)
    {
        const auto *p = static_cast<const unsigned char *>(ptr);
        while (size--) {
            hash ^= *p++;
            hash *= 1099511628211ULL;
        }
    }

    template <class T> void Value(const T &value)
    {
        Hash(&value, sizeof(value));
    }

    bool Includes(const LogicalRect &bounds)
    {
        if (!pass && counters) {
            ++counters->dependency_leaves_checked;
        }
        const bool included = effects::Intersects(bounds, footprint);
        if (!pass && counters) {
            if (included) {
                ++counters->dependency_leaves_included;
            } else {
                ++counters->dependency_leaves_skipped;
            }
        }
        return included;
    }

    void Dependency(wlr_scene_buffer *buffer, const LogicalRect &destination)
    {
        if (paint_generations) {
            if (auto it = paint_generations->find(buffer); it != paint_generations->end()) {
                Value(it->second);
                return;
            }
        }
        auto *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
        if (!scene_surface || !contents) {
            dependency_changed = true;
            return;
        }
        auto it = contents->find(scene_surface->surface);
        if (it == contents->end() || !it->second.initialized) {
            dependency_changed = true;
            return;
        }
        const auto &content = it->second;
        LogicalRect source{0, 0, double(content.mapping.width), double(content.mapping.height)};
        const auto &clip = scene_surface->clip;
        const bool clipped = clip.width > 0 && clip.height > 0;
        if (clipped) {
            source = {double(clip.x), double(clip.y),
                      double(std::min(clip.width, content.mapping.width - clip.x)),
                      double(std::min(clip.height, content.mapping.height - clip.y))};
        }
        // Effective damage is already in logical post-transform/viewport space.
        // Complex clipping combined with a viewport remains conservative.
        auto query = effects::MapRect(footprint, destination, source);
        // wlroots 0.18 effective damage rounds fractional viewport sources,
        // while scene texture sampling retains their floating point extent.
        // Without raw buffer damage projection, that mismatch is unprovable.
        const auto integral = [](double value) {
            return std::isfinite(value) && std::floor(value) == value;
        };
        const bool fractional_source =
            content.mapping.has_source &&
            (!integral(content.mapping.source_x) || !integral(content.mapping.source_y) ||
             !integral(content.mapping.source_width) || !integral(content.mapping.source_height));
        if (!query || fractional_source ||
            (clipped && (content.mapping.has_source || content.mapping.has_destination))) {
            dependency_changed = true;
            Value(content.identity);
            Value(content.history.Revision());
            if (counters) {
                ++counters->damage_history_fallbacks;
            }
            return;
        }
        effects::DependencyStamp old;
        if (previous) {
            if (auto prior = previous->find(buffer); prior != previous->end()) {
                old = prior->second;
            }
        }
        auto observation =
            effects::Observe(content.history, old, content.identity, *query, content.mapping_epoch);
        candidates[buffer] = observation.stamp;
        Value(observation.stamp.identity);
        Value(observation.stamp.sampled_revision);
        dependency_changed |= observation.changed;
        advanced_without_damage |= observation.advanced_without_damage;
        if (observation.fallback && counters) {
            ++counters->damage_history_fallbacks;
        }
    }

    void Node(wlr_scene_node *n, int x = 0, int y = 0)
    {
        if (stopped || n == skip || (excluded && excluded->contains(n)) || !n->enabled) {
            return;
        }
        if (n == target) {
            stopped = true;
            return;
        }
        x += n->x;
        y += n->y;
        if (n->type == WLR_SCENE_NODE_TREE) {
            auto *tree = wlr_scene_tree_from_node(n);
            wlr_scene_node *child;
            wl_list_for_each(child, &tree->children, link)
            {
                Node(child, x, y);
            }
            return;
        }
        if (n->type == WLR_SCENE_NODE_RECT) {
            auto *rect = wlr_scene_rect_from_node(n);
            if (!Includes({double(x), double(y), double(rect->width), double(rect->height)})) {
                return;
            }
            Value(x);
            Value(y);
            Value(n->type);
            Value(rect->width);
            Value(rect->height);
            Hash(rect->color, sizeof(rect->color));
            if (pass) {
                wlr_render_rect_options options{};
                options.box = {int(std::floor((x - origin_x) * scale)),
                               int(std::floor((y - origin_y) * scale)),
                               int(std::ceil(rect->width * scale)),
                               int(std::ceil(rect->height * scale))};
                options.color = {rect->color[0], rect->color[1], rect->color[2], rect->color[3]};
                wlr_render_pass_add_rect(pass, &options);
                if (counters) {
                    ++counters->capture_nodes;
                }
            }
        } else if (n->type == WLR_SCENE_NODE_BUFFER) {
            auto *buffer = wlr_scene_buffer_from_node(n);
            auto *cached = buffer->texture;
            if (!cached && buffer->buffer) {
                if (auto *client = wlr_client_buffer_get(buffer->buffer)) {
                    cached = client->texture;
                }
            }
            if (!buffer->buffer && !cached) {
                return;
            }
            const int width = buffer->dst_width ? buffer->dst_width
                                                : (cached ? cached->width : buffer->buffer->width);
            const int height = buffer->dst_height
                                   ? buffer->dst_height
                                   : (cached ? cached->height : buffer->buffer->height);
            const LogicalRect bounds{double(x), double(y), double(width), double(height)};
            if (!Includes(bounds)) {
                return;
            }
            Value(x);
            Value(y);
            Value(n->type);
            Value(width);
            Value(height);
            Value(buffer->opacity);
            Value(buffer->src_box.x);
            Value(buffer->src_box.y);
            Value(buffer->src_box.width);
            Value(buffer->src_box.height);
            Value(buffer->transform);
            Value(buffer->filter_mode);
            if (!pass) {
                Dependency(buffer, bounds);
            }
            if (pass) {
                auto *texture = cached ? cached : wlr_texture_from_buffer(renderer, buffer->buffer);
                if (!texture) {
                    capture_failed = true;
                    return;
                }
                wlr_render_texture_options options{};
                options.texture = texture;
                options.src_box = buffer->src_box;
                // Scene buffer transform describes buffer-to-surface mapping;
                // texture rendering uses its inverse, as native scene does.
                options.transform = wlr_output_transform_invert(buffer->transform);
                options.filter_mode = buffer->filter_mode;
                options.alpha = &buffer->opacity;
                options.dst_box = {int(std::floor((x - origin_x) * scale)),
                                   int(std::floor((y - origin_y) * scale)),
                                   int(std::ceil(width * scale)), int(std::ceil(height * scale))};
                wlr_render_pass_add_texture(pass, &options);
                if (counters) {
                    ++counters->capture_nodes;
                }
                if (!cached) {
                    textures.push_back(texture);
                }
            }
        }
    }
};

} // namespace effect_detail

using effect_detail::blur_shader;
using effect_detail::Buffer;
using effect_detail::ContextScope;
using effect_detail::LogicalRect;
using effect_detail::material_shader;
using effect_detail::Paint;
using effect_detail::Program;
using effect_detail::Region;
using effect_detail::SurfaceContent;
using effect_detail::SurfaceMapping;
using effect_detail::Walk;

struct SurfaceEffects::Impl {
    struct State {
        Impl *owner{};
        wlr_surface *surface{};
        wl_resource *resource{};
        wl_listener commit{}, destroy{};
        std::vector<Region> pending, current;
        bool dirty{};
    };

    wlr_renderer *renderer{};
    wlr_allocator *allocator{};
    wl_global *global{};
    std::set<wl_resource *> managers, objects;
    std::map<wlr_surface *, std::unique_ptr<State>> states;
    std::map<wlr_surface *, std::vector<std::unique_ptr<Paint>>> paints;
    std::map<wlr_surface *, SurfaceContent> contents;
    std::uint64_t next_surface_identity{};
    GLuint blur{}, material{};
    bool supported{}, needs_update{true};
    std::uint64_t generated{};
    WorkCounters counters;
    std::function<void()> wake_handler;
    void NotifyWake();
    void MarkDirty();
    void NotifySurfaceCommit(wlr_surface *surface);
    void ForgetSurface(wlr_surface *surface);
    Impl(wl_display *display, wlr_renderer *r, wlr_allocator *a);
    ~Impl();
    static State *Get(wl_resource *resource);
    static void Destroy(wl_client *, wl_resource *resource);
    static void Clear(wl_client *, wl_resource *resource);
    static void Add(wl_client *, wl_resource *resource, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w,
                    wl_fixed_t h, wl_fixed_t radius, wl_fixed_t blur_radius);
    static void AddContour(wl_client *, wl_resource *resource, wl_fixed_t blur_radius,
                           wl_array *payload);
    static void EffectGone(wl_resource *resource);
    static void Commit(wl_listener *listener, void *);
    static void SurfaceGone(wl_listener *listener, void *);
    static void NewEffect(wl_client *client, wl_resource *manager, uint32_t id,
                          wl_resource *surface_resource);
    static void ManagerGone(wl_resource *resource);
    static void Bind(wl_client *client, void *data, uint32_t version, uint32_t id);
    std::unique_ptr<Buffer> Allocate(int width, int height);
    bool Draw(Buffer &target, Buffer &source, GLuint program, int width, int height,
              const Region *region = nullptr, double origin_x = 0, double origin_y = 0,
              const contracts::ThemeDecoration *decoration = nullptr, Paint *paint = nullptr);
    bool PrepareMask(Paint &paint, const Region &region, int width, int height, double origin_x,
                     double origin_y);
    bool Blur(Buffer &target, Buffer &source, double radius, bool horizontal);
    UpdateResult Update(wlr_scene *scene, std::span<WlrXdgView *const> views, WlrXdgView *focused,
                        const contracts::ThemeSnapshot *theme, std::span<const Target> popups);
};
} // namespace prism::wm
