#include "prism/wm/surface_effects.hpp"
#include "prism/wm/effect_dependencies.hpp"
#include "prism/wm/xdg_view.hpp"
#include "prism/contracts/surface_effect.hpp"
#include "prism/wm/theme.hpp"
#include "prism/core/logging.hpp"
#include "prism-surface-effects-server.h"
extern "C" {
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/render/allocator.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/transform.h>
#include <drm_fourcc.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

namespace prism::wm {
namespace {
using Region=contracts::SurfaceEffectRegion;
using LogicalRect=effects::Rect;
struct MappingSignature {
    int width{},height{},buffer_width{},buffer_height{},scale{};
    wl_output_transform transform{};
    bool has_buffer{},has_source{},has_destination{};
    double source_x{},source_y{},source_width{},source_height{};
    int destination_width{},destination_height{};
    bool operator==(const MappingSignature&)const=default;
};
MappingSignature SurfaceMapping(wlr_surface* surface){
    const auto& state=surface->current;
    MappingSignature mapping;
    mapping.width=state.width;mapping.height=state.height;
    mapping.buffer_width=state.buffer_width;mapping.buffer_height=state.buffer_height;
    mapping.scale=state.scale;mapping.transform=state.transform;
    mapping.has_buffer=wlr_surface_has_buffer(surface);
    mapping.has_source=state.viewport.has_src;mapping.has_destination=state.viewport.has_dst;
    if(mapping.has_source){mapping.source_x=state.viewport.src.x;mapping.source_y=state.viewport.src.y;
        mapping.source_width=state.viewport.src.width;mapping.source_height=state.viewport.src.height;}
    if(mapping.has_destination){mapping.destination_width=state.viewport.dst_width;mapping.destination_height=state.viewport.dst_height;}
    return mapping;
}
struct SurfaceContent {
    std::uint64_t identity{},mapping_epoch{};
    MappingSignature mapping;
    effects::DamageHistory history;
    bool initialized{};
};
struct ContextScope {
    EGLDisplay old_display=eglGetCurrentDisplay(); EGLContext old_context=eglGetCurrentContext();
    EGLSurface old_draw=eglGetCurrentSurface(EGL_DRAW),old_read=eglGetCurrentSurface(EGL_READ);
    EGLDisplay display{}; bool current{};
    explicit ContextScope(wlr_renderer* renderer) {
        auto* egl=wlr_gles2_renderer_get_egl(renderer); display=wlr_egl_get_display(egl);
        current=eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,wlr_egl_get_context(egl));
    }
    ~ContextScope() {
        if(!current)return;
        if (old_display!=EGL_NO_DISPLAY) eglMakeCurrent(old_display,old_draw,old_read,old_context);
        else eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    }
};
GLuint Shader(GLenum type,const char* source) {
    auto id=glCreateShader(type); glShaderSource(id,1,&source,nullptr); glCompileShader(id);
    GLint success{}; glGetShaderiv(id,GL_COMPILE_STATUS,&success);
    if (!success) { char log[512]{}; glGetShaderInfoLog(id,sizeof(log),nullptr,log); PRISM_LOG_ERROR("SURFACE-EFFECT","shader: %s",log); glDeleteShader(id); return 0; }
    return id;
}
GLuint Program(const char* fragment) {
    const char* vertex="attribute vec2 position; varying vec2 uv; void main(){uv=position;gl_Position=vec4(position*2.0-1.0,0.0,1.0);}";
    auto v=Shader(GL_VERTEX_SHADER,vertex),f=Shader(GL_FRAGMENT_SHADER,fragment);
    if (!v || !f) { if(v)glDeleteShader(v); if(f)glDeleteShader(f); return 0; }
    auto p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glBindAttribLocation(p,0,"position");glLinkProgram(p);
    glDeleteShader(v);glDeleteShader(f);GLint success{};glGetProgramiv(p,GL_LINK_STATUS,&success);
    if (!success) {glDeleteProgram(p);return 0;}return p;
}
constexpr const char* blur_shader=R"(
precision mediump float;varying vec2 uv;uniform sampler2D image;uniform vec2 step_size;
void main(){vec4 c=texture2D(image,uv)*0.227027;
c+=(texture2D(image,uv+step_size*1.384615)+texture2D(image,uv-step_size*1.384615))*0.316216;
c+=(texture2D(image,uv+step_size*3.230769)+texture2D(image,uv-step_size*3.230769))*0.070270;
gl_FragColor=c;})";
constexpr const char* material_shader=R"(
precision mediump float;varying vec2 uv;uniform sampler2D image;uniform vec2 size;
uniform vec4 box;uniform float radius;uniform float shadow;uniform float blur_enabled;
uniform float border_width;uniform vec4 border_color;uniform vec4 shadow_color;uniform float shadow_offset;
float distance_box(vec2 p){vec2 q=abs(p-(box.xy+box.zw*0.5))-(box.zw*0.5-vec2(radius));return length(max(q,0.0))+min(max(q.x,q.y),0.0)-radius;}
void main(){float d=distance_box(uv*size);float inside=1.0-smoothstep(-0.5,0.5,d);
vec4 c=texture2D(image,uv)*inside*blur_enabled;
float sd=max(distance_box(uv*size-vec2(0.0,shadow_offset)),0.0);
float a=shadow>0.0?exp(-sd*sd/(shadow*shadow*0.32))*shadow_color.a*(1.0-inside):0.0;
c+=vec4(shadow_color.rgb*a,a)*(1.0-c.a);
float ring=(1.0-smoothstep(border_width-0.5,border_width+0.5,abs(d)))*(1.0-inside);
vec4 b=vec4(border_color.rgb*border_color.a,border_color.a)*ring;c=b+c*(1.0-b.a);
gl_FragColor=c;})";
struct Buffer {
    wlr_buffer* buffer{};wlr_texture* texture{};
    ~Buffer(){if(texture)wlr_texture_destroy(texture);if(buffer)wlr_buffer_drop(buffer);}
};
struct Paint {
    wlr_scene_tree* tree{};wlr_scene_buffer* node{};
    std::unique_ptr<Buffer> source,intermediate,result;
    int width{},height{};std::uint64_t key{},generation{};bool valid{};
    std::map<wlr_scene_buffer*,effects::DependencyStamp> dependencies;
    ~Paint(){if(tree)wlr_scene_node_destroy(&tree->node);}
};
struct Walk {
    wlr_scene_node* target{};wlr_scene_node* skip{};wlr_renderer* renderer{};wlr_render_pass* pass{};
    double origin_x{},origin_y{},scale{1};bool stopped{},dependency_changed{},advanced_without_damage{},capture_failed{};
    std::uint64_t hash{1469598103934665603ULL};
    LogicalRect footprint;
    const std::set<wlr_scene_node*>* excluded{};
    const std::map<wlr_scene_buffer*,std::uint64_t>* paint_generations{};
    const std::map<wlr_surface*,SurfaceContent>* contents{};
    const std::map<wlr_scene_buffer*,effects::DependencyStamp>* previous{};
    std::map<wlr_scene_buffer*,effects::DependencyStamp> candidates;
    SurfaceEffects::WorkCounters* counters{};
    std::vector<wlr_texture*> textures;
    Walk(wlr_scene_node* target_node,wlr_scene_node* skip_node,wlr_renderer* r,
         wlr_render_pass* render_pass=nullptr,double x=0,double y=0,double factor=1)
        :target(target_node),skip(skip_node),renderer(r),pass(render_pass),
         origin_x(x),origin_y(y),scale(factor){}
    ~Walk(){for(auto* texture:textures)wlr_texture_destroy(texture);}
    void Hash(const void* ptr,std::size_t size) {const auto* p=static_cast<const unsigned char*>(ptr);while(size--){hash^=*p++;hash*=1099511628211ULL;}}
    template<class T>void Value(const T& value){Hash(&value,sizeof(value));}
    bool Includes(const LogicalRect& bounds){
        if(!pass&&counters)++counters->dependency_leaves_checked;
        const bool included=effects::Intersects(bounds,footprint);
        if(!pass&&counters){if(included)++counters->dependency_leaves_included;else ++counters->dependency_leaves_skipped;}
        return included;
    }
    void Dependency(wlr_scene_buffer* buffer,const LogicalRect& destination){
        if(paint_generations)if(auto it=paint_generations->find(buffer);it!=paint_generations->end()){
            Value(it->second);return;
        }
        auto* scene_surface=wlr_scene_surface_try_from_buffer(buffer);
        if(!scene_surface||!contents){dependency_changed=true;return;}
        auto it=contents->find(scene_surface->surface);
        if(it==contents->end()||!it->second.initialized){dependency_changed=true;return;}
        const auto& content=it->second;
        LogicalRect source{0,0,double(content.mapping.width),double(content.mapping.height)};
        const auto& clip=scene_surface->clip;
        const bool clipped=clip.width>0&&clip.height>0;
        if(clipped)source={double(clip.x),double(clip.y),
            double(std::min(clip.width,content.mapping.width-clip.x)),
            double(std::min(clip.height,content.mapping.height-clip.y))};
        // Effective damage is already in logical post-transform/viewport space.
        // Complex clipping combined with a viewport remains conservative.
        auto query=effects::MapRect(footprint,destination,source);
        // wlroots 0.18 effective damage rounds fractional viewport sources,
        // while scene texture sampling retains their floating point extent.
        // Without raw buffer damage projection, that mismatch is unprovable.
        const auto integral=[](double value){return std::isfinite(value)&&std::floor(value)==value;};
        const bool fractional_source=content.mapping.has_source&&
            (!integral(content.mapping.source_x)||!integral(content.mapping.source_y)||
             !integral(content.mapping.source_width)||!integral(content.mapping.source_height));
        if(!query||fractional_source||(clipped&&(content.mapping.has_source||content.mapping.has_destination))){
            dependency_changed=true;Value(content.identity);Value(content.history.Revision());
            if(counters)++counters->damage_history_fallbacks;
            return;
        }
        effects::DependencyStamp old;
        if(previous)if(auto prior=previous->find(buffer);prior!=previous->end())old=prior->second;
        auto observation=effects::Observe(content.history,old,content.identity,*query,content.mapping_epoch);
        candidates[buffer]=observation.stamp;
        Value(observation.stamp.identity);Value(observation.stamp.sampled_revision);
        dependency_changed|=observation.changed;
        advanced_without_damage|=observation.advanced_without_damage;
        if(observation.fallback&&counters)++counters->damage_history_fallbacks;
    }
    void Node(wlr_scene_node* n,int x=0,int y=0) {
        if(stopped||n==skip||(excluded&&excluded->contains(n))||!n->enabled)return;
        if(n==target){stopped=true;return;}
        x+=n->x;y+=n->y;
        if(n->type==WLR_SCENE_NODE_TREE){
            auto* tree=wlr_scene_tree_from_node(n);wlr_scene_node* child;
            wl_list_for_each(child,&tree->children,link)Node(child,x,y);
            return;
        }
        if(n->type==WLR_SCENE_NODE_RECT){
            auto* rect=wlr_scene_rect_from_node(n);
            if(!Includes({double(x),double(y),double(rect->width),double(rect->height)}))return;
            Value(x);Value(y);Value(n->type);Value(rect->width);Value(rect->height);Hash(rect->color,sizeof(rect->color));
            if(pass){wlr_render_rect_options options{};
                options.box={int(std::floor((x-origin_x)*scale)),int(std::floor((y-origin_y)*scale)),int(std::ceil(rect->width*scale)),int(std::ceil(rect->height*scale))};
                options.color={rect->color[0],rect->color[1],rect->color[2],rect->color[3]};
                wlr_render_pass_add_rect(pass,&options);if(counters)++counters->capture_nodes;
            }
        }else if(n->type==WLR_SCENE_NODE_BUFFER){
            auto* buffer=wlr_scene_buffer_from_node(n);
            auto* cached=buffer->texture;
            if(!cached&&buffer->buffer)if(auto* client=wlr_client_buffer_get(buffer->buffer))cached=client->texture;
            if(!buffer->buffer&&!cached)return;
            const int width=buffer->dst_width?buffer->dst_width:(cached?cached->width:buffer->buffer->width);
            const int height=buffer->dst_height?buffer->dst_height:(cached?cached->height:buffer->buffer->height);
            const LogicalRect bounds{double(x),double(y),double(width),double(height)};
            if(!Includes(bounds))return;
            Value(x);Value(y);Value(n->type);Value(width);Value(height);Value(buffer->opacity);
            Value(buffer->src_box.x);Value(buffer->src_box.y);Value(buffer->src_box.width);Value(buffer->src_box.height);
            Value(buffer->transform);Value(buffer->filter_mode);
            if(!pass)Dependency(buffer,bounds);
            if(pass){
                auto* texture=cached?cached:wlr_texture_from_buffer(renderer,buffer->buffer);
                if(!texture){capture_failed=true;return;}
                wlr_render_texture_options options{};options.texture=texture;options.src_box=buffer->src_box;
                // Scene buffer transform describes buffer-to-surface mapping;
                // texture rendering uses its inverse, as native scene does.
                options.transform=wlr_output_transform_invert(buffer->transform);
                options.filter_mode=buffer->filter_mode;options.alpha=&buffer->opacity;
                options.dst_box={int(std::floor((x-origin_x)*scale)),int(std::floor((y-origin_y)*scale)),int(std::ceil(width*scale)),int(std::ceil(height*scale))};
                wlr_render_pass_add_texture(pass,&options);if(counters)++counters->capture_nodes;
                if(!cached)textures.push_back(texture);
            }
        }
    }
};
}
struct SurfaceEffects::Impl {
    struct State { Impl* owner{};wlr_surface* surface{};wl_resource* resource{};wl_listener commit{},destroy{};std::vector<Region> pending,current;bool dirty{}; };
    wlr_renderer* renderer{};wlr_allocator* allocator{};wl_global* global{};
    std::set<wl_resource*> managers,objects;
    std::map<wlr_surface*,std::unique_ptr<State>> states;
    std::map<wlr_surface*,std::vector<std::unique_ptr<Paint>>> paints;
    std::map<wlr_surface*,SurfaceContent> contents;
    std::uint64_t next_surface_identity{};
    GLuint blur{},material{};bool supported{},needs_update{true};std::uint64_t generated{};
    WorkCounters counters;
    std::function<void()> wake_handler;
    void NotifyWake(){if(wake_handler){++counters.wake_notifications;wake_handler();}}
    void MarkDirty(){if(!needs_update){needs_update=true;++counters.dirty_transitions;NotifyWake();}}
    void NotifySurfaceCommit(wlr_surface* surface){
        if(!surface)return;
        auto& content=contents[surface];
        if(!content.identity)content.identity=++next_surface_identity;
        const auto mapping=SurfaceMapping(surface);
        const bool mapping_changed=!content.initialized||content.mapping!=mapping||
            (surface->current.committed&WLR_SURFACE_STATE_OFFSET);
        const bool first_content=mapping.has_buffer&&(!content.initialized||!content.mapping.has_buffer);
        if(mapping_changed)++content.mapping_epoch;
        content.mapping=mapping;content.initialized=true;
        constexpr auto damage_fields=WLR_SURFACE_STATE_BUFFER|WLR_SURFACE_STATE_SURFACE_DAMAGE|WLR_SURFACE_STATE_BUFFER_DAMAGE;
        std::vector<LogicalRect> rectangles;
        if(mapping.has_buffer&&(surface->current.committed&damage_fields)){
            pixman_region32_t damage;pixman_region32_init(&damage);
            wlr_surface_get_effective_damage(surface,&damage);
            int count{};const auto* boxes=pixman_region32_rectangles(&damage,&count);
            // Bounded copied storage; extents conservatively contain every box.
            if(count>int(effects::DamageHistory::kMaxRectangles)){
                const auto* box=pixman_region32_extents(&damage);
                rectangles.push_back({double(box->x1),double(box->y1),double(box->x2-box->x1),double(box->y2-box->y1)});
            }else for(int i=0;i<count;++i)rectangles.push_back({double(boxes[i].x1),double(boxes[i].y1),double(boxes[i].x2-boxes[i].x1),double(boxes[i].y2-boxes[i].y1)});
            pixman_region32_fini(&damage);
        }
        const bool recorded=content.history.Record(rectangles,content.mapping_epoch,first_content);
        if(recorded)++counters.content_revisions;
        else ++counters.metadata_commits;
        if(recorded||mapping_changed)MarkDirty();
    }
    void ForgetSurface(wlr_surface* surface){if(contents.erase(surface))MarkDirty();}
    Impl(wl_display* display,wlr_renderer* r,wlr_allocator* a):renderer(r),allocator(a) {
        supported=wlr_renderer_is_gles2(renderer);
        if(supported){ContextScope context(renderer);if(context.current){blur=Program(blur_shader);material=Program(material_shader);}supported=context.current && blur && material;}
        global=wl_global_create(display,&prism_surface_effect_manager_v1_interface,1,this,Bind);
        PRISM_LOG_INFO("SURFACE-EFFECT","GPU backdrop capability=%d (typed protocol v1)",supported);
    }
    ~Impl(){wake_handler={};while(!objects.empty())wl_resource_destroy(*objects.begin());while(!managers.empty())wl_resource_destroy(*managers.begin());
        for(auto& [surface,s]:states){wl_list_remove(&s->commit.link);wl_list_remove(&s->destroy.link);}states.clear();paints.clear();
        if(global)wl_global_destroy(global);
        if(wlr_renderer_is_gles2(renderer)){ContextScope context(renderer);if(context.current){if(blur)glDeleteProgram(blur);if(material)glDeleteProgram(material);}}}
    static State* Get(wl_resource* resource){return static_cast<State*>(wl_resource_get_user_data(resource));}
    static void Destroy(wl_client*,wl_resource* resource){wl_resource_destroy(resource);}
    static void Clear(wl_client*,wl_resource* resource){if(auto* s=Get(resource)){s->pending.clear();s->dirty=true;}}
    static void Add(wl_client*,wl_resource* resource,wl_fixed_t x,wl_fixed_t y,wl_fixed_t w,wl_fixed_t h,wl_fixed_t radius,wl_fixed_t blur_radius){
        auto* s=Get(resource);if(!s){wl_resource_post_error(resource,1,"Surface has been destroyed");return;}
        Region r{{wl_fixed_to_double(x),wl_fixed_to_double(y),wl_fixed_to_double(w),wl_fixed_to_double(h)},wl_fixed_to_double(radius),wl_fixed_to_double(blur_radius)};
        if(s->pending.size()>=8 || std::abs(r.bounds.x)>8192 || std::abs(r.bounds.y)>8192 || r.bounds.width<=0 || r.bounds.height<=0 || r.bounds.width>8192 || r.bounds.height>8192 || r.corner_radius<0 || r.corner_radius>256 || r.blur_radius<0 || r.blur_radius>48){wl_resource_post_error(resource,0,"Invalid surface effect region");return;}
        s->pending.push_back(r);s->dirty=true;
    }
    static void EffectGone(wl_resource* resource){auto* s=Get(resource);if(s){s->owner->objects.erase(resource);s->resource=nullptr;s->pending.clear();s->dirty=true;s->owner->MarkDirty();}}
    static void Commit(wl_listener* listener,void*){auto* s=reinterpret_cast<State*>(reinterpret_cast<char*>(listener)-offsetof(State,commit));if(s->dirty){s->current=s->pending;s->dirty=false;s->owner->MarkDirty();}}
    static void SurfaceGone(wl_listener* listener,void*){auto* s=reinterpret_cast<State*>(reinterpret_cast<char*>(listener)-offsetof(State,destroy));auto* owner=s->owner;auto* surface=s->surface;
        wl_list_remove(&s->commit.link);wl_list_remove(&s->destroy.link);if(s->resource){owner->objects.erase(s->resource);wl_resource_set_user_data(s->resource,nullptr);}
        if(auto it=owner->paints.find(surface);it!=owner->paints.end()){owner->counters.removed_regions+=it->second.size();owner->paints.erase(it);}
        owner->states.erase(surface);owner->MarkDirty();}
    static void NewEffect(wl_client* client,wl_resource* manager,uint32_t id,wl_resource* surface_resource){
        auto* owner=static_cast<Impl*>(wl_resource_get_user_data(manager));auto* surface=wlr_surface_from_resource(surface_resource);
        auto& state=owner->states[surface];if(!state){state=std::make_unique<State>();state->owner=owner;state->surface=surface;state->commit.notify=Commit;state->destroy.notify=SurfaceGone;wl_signal_add(&surface->events.commit,&state->commit);wl_signal_add(&surface->events.destroy,&state->destroy);}
        if(state->resource){wl_resource_post_error(manager,0,"Surface already has an effect object");return;}
        auto* resource=wl_resource_create(client,&prism_surface_effect_v1_interface,1,id);if(!resource){wl_client_post_no_memory(client);return;}
        static const struct prism_surface_effect_v1_interface impl{Destroy,Clear,Add};
        state->resource=resource;owner->objects.insert(resource);wl_resource_set_implementation(resource,&impl,state.get(),EffectGone);
    }
    static void ManagerGone(wl_resource* resource){static_cast<Impl*>(wl_resource_get_user_data(resource))->managers.erase(resource);}
    static void Bind(wl_client* client,void* data,uint32_t version,uint32_t id){auto* owner=static_cast<Impl*>(data);auto* resource=wl_resource_create(client,&prism_surface_effect_manager_v1_interface,version,id);if(!resource){wl_client_post_no_memory(client);return;}
        static const struct prism_surface_effect_manager_v1_interface impl{Destroy,NewEffect};owner->managers.insert(resource);wl_resource_set_implementation(resource,&impl,owner,ManagerGone);prism_surface_effect_manager_v1_send_capabilities(resource,owner->supported);}
    std::unique_ptr<Buffer> Allocate(int width,int height){++counters.allocation_attempts;
        auto fail=[this](){++counters.allocation_failures;return std::unique_ptr<Buffer>{};};
        auto* formats=wlr_renderer_get_texture_formats(renderer,allocator->buffer_caps);auto* fmt=wlr_drm_format_set_get(formats,DRM_FORMAT_ARGB8888);if(!fmt)return fail();
        auto result=std::make_unique<Buffer>();result->buffer=wlr_allocator_create_buffer(allocator,width,height,fmt);if(!result->buffer)return fail();
        // Initialize imported render target before creating its sampling texture.
        auto* pass=wlr_renderer_begin_buffer_pass(renderer,result->buffer,nullptr);if(!pass)return fail();wlr_render_rect_options clear{};clear.box={0,0,width,height};clear.blend_mode=WLR_RENDER_BLEND_MODE_NONE;wlr_render_pass_add_rect(pass,&clear);if(!wlr_render_pass_submit(pass))return fail();
        result->texture=wlr_texture_from_buffer(renderer,result->buffer);if(!result->texture)return fail();++counters.allocated_buffers;return result;}
    bool Draw(Buffer& target,Buffer& source,GLuint program,int width,int height,const Region* region=nullptr,double padding=0,const contracts::ThemeDecoration* decoration=nullptr){
        ++counters.material_pass_attempts;
        auto* pass=wlr_renderer_begin_buffer_pass(renderer,target.buffer,nullptr);if(!pass)return false;
        wlr_gles2_texture_attribs attrib{};wlr_gles2_texture_get_attribs(source.texture,&attrib);
        if(attrib.target!=GL_TEXTURE_2D){wlr_render_pass_submit(pass);return false;}
        glUseProgram(program);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glBindBuffer(GL_ARRAY_BUFFER,0);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,attrib.tex);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glUniform1i(glGetUniformLocation(program,"image"),0);
        if(region){glUniform2f(glGetUniformLocation(program,"size"),width,height);glUniform4f(glGetUniformLocation(program,"box"),padding,padding,region->bounds.width,region->bounds.height);
            glUniform1f(glGetUniformLocation(program,"radius"),std::min({region->corner_radius,region->bounds.width/2,region->bounds.height/2}));glUniform1f(glGetUniformLocation(program,"shadow"),decoration?decoration->shadow_blur:0);glUniform1f(glGetUniformLocation(program,"blur_enabled"),region->blur_radius>0?1:0);
            const auto border=decoration?decoration->border:contracts::Color{};
            glUniform1f(glGetUniformLocation(program,"border_width"),decoration?decoration->border_width:0);glUniform4f(glGetUniformLocation(program,"border_color"),border.r/255.f,border.g/255.f,border.b/255.f,decoration?border.a/255.f:0);
            const auto shade=decoration?decoration->shadow:contracts::Color{};glUniform4f(glGetUniformLocation(program,"shadow_color"),shade.r/255.f,shade.g/255.f,shade.b/255.f,shade.a/255.f);glUniform1f(glGetUniformLocation(program,"shadow_offset"),decoration?decoration->shadow_y:0);}
        static const GLfloat quad[]{0,0,1,0,0,1,1,1};glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,quad);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glDisableVertexAttribArray(0);glUseProgram(0);glBindTexture(GL_TEXTURE_2D,0);
        bool ok=glGetError()==GL_NO_ERROR;const bool submitted=wlr_render_pass_submit(pass)&&ok;
        if(submitted)++counters.material_passes;
        return submitted;
    }
    bool Blur(Buffer& target,Buffer& source,double radius,bool horizontal){++counters.blur_pass_attempts;auto* pass=wlr_renderer_begin_buffer_pass(renderer,target.buffer,nullptr);if(!pass)return false;
        wlr_gles2_texture_attribs attr{};wlr_gles2_texture_get_attribs(source.texture,&attr);if(attr.target!=GL_TEXTURE_2D){wlr_render_pass_submit(pass);return false;}
        glUseProgram(blur);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glBindBuffer(GL_ARRAY_BUFFER,0);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,attr.tex);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glUniform1i(glGetUniformLocation(blur,"image"),0);
        // Two separable passes at half resolution; sample footprint is logical.
        glUniform2f(glGetUniformLocation(blur,"step_size"),horizontal?radius/(6.46*source.buffer->width):0,horizontal?0:radius/(6.46*source.buffer->height));
        static const GLfloat quad[]{0,0,1,0,0,1,1,1};glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,quad);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glDisableVertexAttribArray(0);glUseProgram(0);glBindTexture(GL_TEXTURE_2D,0);bool ok=glGetError()==GL_NO_ERROR;
        const bool submitted=wlr_render_pass_submit(pass)&&ok;if(submitted)++counters.blur_passes;return submitted;
    }
    UpdateResult Update(wlr_scene* scene,std::span<WlrXdgView* const> views,WlrXdgView* focused,
                const contracts::ThemeSnapshot* theme) {
        ++counters.update_calls;
        if(!needs_update){++counters.skipped_updates;return {};}
        needs_update=false;
        if(!supported){++counters.unsupported_updates;return {};}
        UpdateResult result{true,false};
        std::set<wlr_surface*> alive;
        std::map<wlr_scene_node*,WlrXdgView*> by_node;
        for(auto* view:views)if(view->mapped && view->visible) {
            alive.insert(view->toplevel->base->surface);
            by_node.emplace(&view->scene_tree->node,view);
        }
        // Retire hidden materials before sampling, otherwise old workspace
        // paints could appear as backdrop siblings of a now hidden client.
        for(auto it=paints.begin();it!=paints.end();)if(!alive.contains(it->first)){
            counters.removed_regions+=it->second.size();result.scene_changed|=!it->second.empty();it=paints.erase(it);
        }else ++it;
        std::map<wlr_scene_buffer*,std::uint64_t> paint_generations;
        for(const auto& [surface,list]:paints)for(const auto& paint:list)
            if(paint->node)paint_generations[paint->node]=paint->generation;
        std::vector<WlrXdgView*> ordered;
        auto visit=[&](auto&& self,wlr_scene_node* node)->void {
            if(auto it=by_node.find(node);it!=by_node.end())ordered.push_back(it->second);
            if(node->type==WLR_SCENE_NODE_TREE) {
                auto* tree=wlr_scene_tree_from_node(node);wlr_scene_node* child;
                wl_list_for_each(child,&tree->children,link)self(self,child);
            }
        };
        visit(visit,&scene->tree.node);
        // Resolve and paint in actual lower-to-upper scene order. A reused GPU
        // buffer carries a content generation, not just its stable pointer.
        for(auto* view:ordered) {
            auto* surface=view->toplevel->base->surface;
            const auto state=ResolveDecoration(theme,view==focused,view->fullscreen,view->shell_role!=0);
            const auto& style=state.style;
            std::vector<Region> regions;
            if(auto it=states.find(surface);it!=states.end())regions=it->second->current;
            auto is_frame=[&](const Region& r){return style.enabled && std::abs(r.bounds.x)<.01 && std::abs(r.bounds.y)<.01 && std::abs(r.bounds.width-view->width)<.01 && std::abs(r.bounds.height-view->height)<.01;};
            if(style.enabled && std::none_of(regions.begin(),regions.end(),is_frame))
                regions.insert(regions.begin(),{{0,0,double(view->width),double(view->height)},style.radius,0});
            auto& list=paints[surface];
            while(list.size()>regions.size()) {paint_generations.erase(list.back()->node);list.pop_back();++counters.removed_regions;result.scene_changed=true;}
            while(list.size()<regions.size())list.push_back(std::make_unique<Paint>());
            for(auto& paint:list)if(!paint->tree){
                paint->tree=wlr_scene_tree_create(view->scene_tree->node.parent);
                paint->node=wlr_scene_buffer_create(paint->tree,nullptr);
                paint->node->point_accepts_input=[](wlr_scene_buffer*,double*,double*){return false;};
                result.scene_changed=true;
            }
            // Anchor from the client backwards. Moving a successor later in
            // forward order could separate earlier paints from their client.
            auto* successor=&view->scene_tree->node;
            for(auto it=list.rbegin();it!=list.rend();++it){
                auto* node=&(*it)->tree->node;
                if(node->link.next!=&successor->link){
                    wlr_scene_node_place_below(node,successor);++counters.scene_reorders;result.scene_changed=true;
                }
                successor=node;
            }
            for(std::size_t i=0;i<regions.size();++i) {
                ++counters.regions_checked;
                auto r=regions[i];
                const bool decoration=is_frame(r);
                if(decoration)r.corner_radius=style.radius;
                auto& p=*list[i];
                const double decoration_extent=decoration?std::max(style.border_width,style.shadow_blur+std::abs(style.shadow_y)):0;
                const int padding=int(std::ceil(std::max(r.blur_radius,decoration_extent)));
                const int w=int(std::ceil(r.bounds.width))+2*padding,h=int(std::ceil(r.bounds.height))+2*padding;
                auto fail=[&](){++counters.failed_regions;p.valid=false;
                    if(p.tree && p.tree->node.enabled){wlr_scene_node_set_enabled(&p.tree->node,false);result.scene_changed=true;}};
                if(w<=0||h<=0||w>8192||h>8192) {++counters.invalid_regions;fail();continue;}
                if(!p.tree->node.enabled){wlr_scene_node_set_enabled(&p.tree->node,true);result.scene_changed=true;}
                const int x=view->x+int(std::floor(r.bounds.x))-padding,y=view->y+int(std::floor(r.bounds.y))-padding;
                if(p.tree->node.x!=x||p.tree->node.y!=y){wlr_scene_node_set_position(&p.tree->node,x,y);result.scene_changed=true;}
                std::set<wlr_scene_node*> own;
                for(const auto& paint:list)if(paint->tree)own.insert(&paint->tree->node);
                Walk walk{&view->scene_tree->node,&p.tree->node,renderer};
                walk.excluded=&own;walk.paint_generations=&paint_generations;
                walk.footprint=effects::CaptureFootprint(x,y,w,h);
                walk.contents=&contents;walk.previous=&p.dependencies;walk.counters=&counters;
                if(r.blur_radius>0)walk.Node(&scene->tree.node);
                walk.Value(r.bounds.x);walk.Value(r.bounds.y);walk.Value(r.bounds.width);walk.Value(r.bounds.height);
                walk.Hash(&r.corner_radius,sizeof(r.corner_radius));
                walk.Hash(&r.blur_radius,sizeof(r.blur_radius));walk.Hash(&decoration,sizeof(decoration));
                // Hash individual fields to avoid compiler padding in structs.
                if(decoration){
                    walk.Hash(&style.enabled,sizeof(style.enabled));walk.Hash(&style.radius,sizeof(style.radius));
                    walk.Hash(&style.border_width,sizeof(style.border_width));walk.Hash(&style.border,sizeof(style.border));
                    walk.Hash(&style.shadow_blur,sizeof(style.shadow_blur));walk.Hash(&style.shadow_y,sizeof(style.shadow_y));
                    walk.Hash(&style.shadow,sizeof(style.shadow));
                }
                walk.Hash(&x,sizeof(x));walk.Hash(&y,sizeof(y));
                if(p.valid&&!walk.dependency_changed&&p.key==walk.hash&&p.width==w&&p.height==h){
                    ++counters.cache_hits;
                    if(walk.advanced_without_damage)++counters.partial_damage_cache_hits;
                    p.dependencies=std::move(walk.candidates);continue;
                }
                ++counters.cache_misses;
                if(p.width!=w||p.height!=h||!p.source||!p.intermediate||!p.result) {
                    p.source=Allocate((w+1)/2,(h+1)/2);p.intermediate=Allocate((w+1)/2,(h+1)/2);p.result=Allocate(w,h);
                    p.width=w;p.height=h;
                    if(!p.source||!p.intermediate||!p.result) {PRISM_LOG_ERROR("SURFACE-EFFECT","Buffer allocation failed");fail();continue;}
                }
                if(r.blur_radius>0) {
                    ++counters.capture_pass_attempts;
                    auto* pass=wlr_renderer_begin_buffer_pass(renderer,p.source->buffer,nullptr);
                    if(!pass) {fail();PRISM_LOG_ERROR("SURFACE-EFFECT","Capture pass failed");continue;}
                    wlr_render_rect_options clear{};clear.box={0,0,p.source->buffer->width,p.source->buffer->height};
                    clear.blend_mode=WLR_RENDER_BLEND_MODE_NONE;wlr_render_pass_add_rect(pass,&clear);
                    Walk capture{&view->scene_tree->node,&p.tree->node,renderer,pass,double(x),double(y),.5};
                    capture.excluded=&own;capture.footprint=walk.footprint;capture.counters=&counters;capture.Node(&scene->tree.node);
                    const bool submitted=wlr_render_pass_submit(pass);
                    const bool captured=submitted&&!capture.capture_failed;if(captured)++counters.capture_passes;
                    if(!captured||!Blur(*p.intermediate,*p.source,r.blur_radius/2,true)||!Blur(*p.source,*p.intermediate,r.blur_radius/2,false)) {
                        fail();PRISM_LOG_ERROR("SURFACE-EFFECT","Capture or blur submission failed");continue;
                    }
                }
                if(!Draw(*p.result,*p.source,material,w,h,&r,padding,decoration?&style:nullptr)) {
                    fail();PRISM_LOG_ERROR("SURFACE-EFFECT","Material pass failed");continue;
                }
                wlr_scene_buffer_set_buffer(p.node,p.result->buffer);p.key=walk.hash;p.valid=true;
                p.dependencies=std::move(walk.candidates);
                p.generation=++generated;paint_generations[p.node]=p.generation;
                ++counters.rendered_regions;counters.rendered_pixels+=std::uint64_t(w)*std::uint64_t(h);result.scene_changed=true;
                if(generated<=12)PRISM_LOG_INFO("SURFACE-EFFECT","Rendered region %dx%d blur=%.1f lower-scene-only shell=%d",w,h,r.blur_radius,view->shell_role);
            }
        }
        return result;
    }
};
SurfaceEffects::SurfaceEffects(wl_display* d,wlr_renderer* r,wlr_allocator* a):impl_(std::make_unique<Impl>(d,r,a)){}
SurfaceEffects::~SurfaceEffects()=default;
bool SurfaceEffects::Supported()const{return impl_->supported;}
void SurfaceEffects::SetWakeHandler(std::function<void()> handler){impl_->wake_handler=std::move(handler);if(impl_->needs_update)impl_->NotifyWake();}
void SurfaceEffects::MarkDirty(){impl_->MarkDirty();}
void SurfaceEffects::NotifySurfaceCommit(wlr_surface* surface){impl_->NotifySurfaceCommit(surface);}
void SurfaceEffects::ForgetSurface(wlr_surface* surface){impl_->ForgetSurface(surface);}
bool SurfaceEffects::NeedsUpdate()const{return impl_->needs_update;}
const SurfaceEffects::WorkCounters& SurfaceEffects::Counters()const{return impl_->counters;}
SurfaceEffects::UpdateResult SurfaceEffects::Update(wlr_scene* s,std::span<WlrXdgView* const> v,WlrXdgView* f,const contracts::ThemeSnapshot* t){return impl_->Update(s,v,f,t);}
}
