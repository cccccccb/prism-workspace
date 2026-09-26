#include "prism/wm/surface_effects.hpp"
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
    int width{},height{};std::uint64_t key{},generation{};
    ~Paint(){if(tree)wlr_scene_node_destroy(&tree->node);}
};
struct Walk {
    wlr_scene_node* target{};wlr_scene_node* skip{};wlr_renderer* renderer{};wlr_render_pass* pass{};
    double origin_x{},origin_y{},scale{1};bool stopped{};std::uint64_t hash{1469598103934665603ULL};
    const std::set<wlr_scene_node*>* excluded{};
    const std::map<wlr_scene_buffer*,std::uint64_t>* paint_generations{};
    std::vector<wlr_texture*> textures;
    Walk(wlr_scene_node* target_node,wlr_scene_node* skip_node,wlr_renderer* r,
         wlr_render_pass* render_pass=nullptr,double x=0,double y=0,double factor=1)
        :target(target_node),skip(skip_node),renderer(r),pass(render_pass),
         origin_x(x),origin_y(y),scale(factor){}
    ~Walk(){for(auto* texture:textures)wlr_texture_destroy(texture);}
    void Hash(const void* ptr,std::size_t size) {const auto* p=static_cast<const unsigned char*>(ptr);while(size--){hash^=*p++;hash*=1099511628211ULL;}}
    void Node(wlr_scene_node* n,int x=0,int y=0) {
        if(stopped || n==skip || (excluded && excluded->contains(n)) || !n->enabled)return;
        if(n==target){stopped=true;return;}
        x+=n->x;y+=n->y;Hash(&x,sizeof(x));Hash(&y,sizeof(y));Hash(&n->type,sizeof(n->type));
        if(n->type==WLR_SCENE_NODE_TREE){auto* t=wlr_scene_tree_from_node(n);wlr_scene_node* c;wl_list_for_each(c,&t->children,link)Node(c,x,y);}
        else if(n->type==WLR_SCENE_NODE_RECT){auto* r=wlr_scene_rect_from_node(n);Hash(&r->width,sizeof(r->width));Hash(&r->height,sizeof(r->height));Hash(r->color,sizeof(r->color));
            if(pass){wlr_render_rect_options o{};o.box={int(std::floor((x-origin_x)*scale)),int(std::floor((y-origin_y)*scale)),int(std::ceil(r->width*scale)),int(std::ceil(r->height*scale))};o.color={r->color[0],r->color[1],r->color[2],r->color[3]};wlr_render_pass_add_rect(pass,&o);}}
        else if(n->type==WLR_SCENE_NODE_BUFFER){auto* b=wlr_scene_buffer_from_node(n);
            auto* cached=b->texture;
            if(!cached && b->buffer){if(auto* client=wlr_client_buffer_get(b->buffer))cached=client->texture;}
            if(!b->buffer && !cached)return;
            Hash(&b->buffer,sizeof(b->buffer));Hash(&cached,sizeof(cached));Hash(&b->opacity,sizeof(b->opacity));Hash(&b->src_box,sizeof(b->src_box));Hash(&b->dst_width,sizeof(b->dst_width));Hash(&b->dst_height,sizeof(b->dst_height));Hash(&b->transform,sizeof(b->transform));
            if(auto* s=wlr_scene_surface_try_from_buffer(b))Hash(&s->surface->current.seq,sizeof(s->surface->current.seq));
            if(paint_generations)if(auto it=paint_generations->find(b);it!=paint_generations->end())Hash(&it->second,sizeof(it->second));
            if(pass){auto* tex=cached?cached:wlr_texture_from_buffer(renderer,b->buffer);if(!tex)return;
                wlr_render_texture_options o{};o.texture=tex;o.src_box=b->src_box;o.transform=b->transform;o.alpha=&b->opacity;
                o.dst_box={int(std::floor((x-origin_x)*scale)),int(std::floor((y-origin_y)*scale)),int(std::ceil((b->dst_width?b->dst_width:tex->width)*scale)),int(std::ceil((b->dst_height?b->dst_height:tex->height)*scale))};wlr_render_pass_add_texture(pass,&o);if(!cached)textures.push_back(tex);}}
    }
};
}
struct SurfaceEffects::Impl {
    struct State { Impl* owner{};wlr_surface* surface{};wl_resource* resource{};wl_listener commit{},destroy{};std::vector<Region> pending,current;bool dirty{}; };
    wlr_renderer* renderer{};wlr_allocator* allocator{};wl_global* global{};
    std::set<wl_resource*> managers,objects;
    std::map<wlr_surface*,std::unique_ptr<State>> states;
    std::map<wlr_surface*,std::vector<std::unique_ptr<Paint>>> paints;
    GLuint blur{},material{};bool supported{};std::uint64_t generated{};
    Impl(wl_display* display,wlr_renderer* r,wlr_allocator* a):renderer(r),allocator(a) {
        supported=wlr_renderer_is_gles2(renderer);
        if(supported){ContextScope context(renderer);if(context.current){blur=Program(blur_shader);material=Program(material_shader);}supported=context.current && blur && material;}
        global=wl_global_create(display,&prism_surface_effect_manager_v1_interface,1,this,Bind);
        PRISM_LOG_INFO("SURFACE-EFFECT","GPU backdrop capability=%d (typed protocol v1)",supported);
    }
    ~Impl(){while(!objects.empty())wl_resource_destroy(*objects.begin());while(!managers.empty())wl_resource_destroy(*managers.begin());
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
    static void EffectGone(wl_resource* resource){auto* s=Get(resource);if(s){s->owner->objects.erase(resource);s->resource=nullptr;s->pending.clear();s->dirty=true;}}
    static void Commit(wl_listener* listener,void*){auto* s=reinterpret_cast<State*>(reinterpret_cast<char*>(listener)-offsetof(State,commit));if(s->dirty){s->current=s->pending;s->dirty=false;}}
    static void SurfaceGone(wl_listener* listener,void*){auto* s=reinterpret_cast<State*>(reinterpret_cast<char*>(listener)-offsetof(State,destroy));auto* owner=s->owner;auto* surface=s->surface;
        wl_list_remove(&s->commit.link);wl_list_remove(&s->destroy.link);if(s->resource){owner->objects.erase(s->resource);wl_resource_set_user_data(s->resource,nullptr);}owner->paints.erase(surface);owner->states.erase(surface);}
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
    std::unique_ptr<Buffer> Allocate(int width,int height){auto* formats=wlr_renderer_get_texture_formats(renderer,allocator->buffer_caps);auto* fmt=wlr_drm_format_set_get(formats,DRM_FORMAT_ARGB8888);if(!fmt)return {};
        auto result=std::make_unique<Buffer>();result->buffer=wlr_allocator_create_buffer(allocator,width,height,fmt);if(!result->buffer)return {};
        // Initialize imported render target before creating its sampling texture.
        auto* pass=wlr_renderer_begin_buffer_pass(renderer,result->buffer,nullptr);if(!pass)return {};wlr_render_rect_options clear{};clear.box={0,0,width,height};clear.blend_mode=WLR_RENDER_BLEND_MODE_NONE;wlr_render_pass_add_rect(pass,&clear);if(!wlr_render_pass_submit(pass))return {};
        result->texture=wlr_texture_from_buffer(renderer,result->buffer);if(!result->texture)return {};return result;}
    bool Draw(Buffer& target,Buffer& source,GLuint program,int width,int height,const Region* region=nullptr,double padding=0,const contracts::ThemeDecoration* decoration=nullptr){
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
        bool ok=glGetError()==GL_NO_ERROR;return wlr_render_pass_submit(pass)&&ok;
    }
    bool Blur(Buffer& target,Buffer& source,double radius,bool horizontal){auto* pass=wlr_renderer_begin_buffer_pass(renderer,target.buffer,nullptr);if(!pass)return false;
        wlr_gles2_texture_attribs attr{};wlr_gles2_texture_get_attribs(source.texture,&attr);if(attr.target!=GL_TEXTURE_2D){wlr_render_pass_submit(pass);return false;}
        glUseProgram(blur);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glBindBuffer(GL_ARRAY_BUFFER,0);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,attr.tex);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glUniform1i(glGetUniformLocation(blur,"image"),0);
        // Two separable passes at half resolution; sample footprint is logical.
        glUniform2f(glGetUniformLocation(blur,"step_size"),horizontal?radius/(6.46*source.buffer->width):0,horizontal?0:radius/(6.46*source.buffer->height));
        static const GLfloat quad[]{0,0,1,0,0,1,1,1};glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,quad);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glDisableVertexAttribArray(0);glUseProgram(0);glBindTexture(GL_TEXTURE_2D,0);bool ok=glGetError()==GL_NO_ERROR;return wlr_render_pass_submit(pass)&&ok;
    }
    void Update(wlr_scene* scene,std::span<WlrXdgView* const> views,WlrXdgView* focused,
                const contracts::ThemeSnapshot* theme) {
        if(!supported)return;
        std::set<wlr_surface*> alive;
        std::map<wlr_scene_node*,WlrXdgView*> by_node;
        for(auto* view:views)if(view->mapped && view->visible) {
            alive.insert(view->toplevel->base->surface);
            by_node.emplace(&view->scene_tree->node,view);
        }
        // Retire hidden materials before sampling, otherwise old workspace
        // paints could appear as backdrop siblings of a now hidden client.
        for(auto it=paints.begin();it!=paints.end();)if(!alive.contains(it->first))it=paints.erase(it);else ++it;
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
            while(list.size()>regions.size()) {paint_generations.erase(list.back()->node);list.pop_back();}
            for(std::size_t i=0;i<regions.size();++i) {
                auto r=regions[i];
                const bool decoration=is_frame(r);
                if(decoration)r.corner_radius=style.radius;
                if(i==list.size())list.push_back(std::make_unique<Paint>());
                auto& p=*list[i];
                const double decoration_extent=decoration?std::max(style.border_width,style.shadow_blur+std::abs(style.shadow_y)):0;
                const int padding=int(std::ceil(std::max(r.blur_radius,decoration_extent)));
                const int w=int(std::ceil(r.bounds.width))+2*padding,h=int(std::ceil(r.bounds.height))+2*padding;
                if(w<=0||h<=0||w>8192||h>8192) {if(p.tree)wlr_scene_node_set_enabled(&p.tree->node,false);continue;}
                if(!p.tree) {
                    p.tree=wlr_scene_tree_create(view->scene_tree->node.parent);
                    p.node=wlr_scene_buffer_create(p.tree,nullptr);
                    p.node->point_accepts_input=[](wlr_scene_buffer*,double*,double*){return false;};
                }
                wlr_scene_node_set_enabled(&p.tree->node,true);
                wlr_scene_node_place_below(&p.tree->node,&view->scene_tree->node);
                const int x=view->x+int(std::floor(r.bounds.x))-padding,y=view->y+int(std::floor(r.bounds.y))-padding;
                wlr_scene_node_set_position(&p.tree->node,x,y);
                std::set<wlr_scene_node*> own;
                for(const auto& paint:list)if(paint->tree)own.insert(&paint->tree->node);
                Walk walk{&view->scene_tree->node,&p.tree->node,renderer};
                walk.excluded=&own;walk.paint_generations=&paint_generations;
                if(r.blur_radius>0)walk.Node(&scene->tree.node);
                walk.Hash(&r.bounds,sizeof(r.bounds));walk.Hash(&r.corner_radius,sizeof(r.corner_radius));
                walk.Hash(&r.blur_radius,sizeof(r.blur_radius));walk.Hash(&decoration,sizeof(decoration));
                walk.Hash(&state.generation,sizeof(state.generation));
                // Hash individual fields to avoid compiler padding in structs.
                walk.Hash(&style.enabled,sizeof(style.enabled));walk.Hash(&style.radius,sizeof(style.radius));
                walk.Hash(&style.border_width,sizeof(style.border_width));walk.Hash(&style.border,sizeof(style.border));
                walk.Hash(&style.shadow_blur,sizeof(style.shadow_blur));walk.Hash(&style.shadow_y,sizeof(style.shadow_y));
                walk.Hash(&style.shadow,sizeof(style.shadow));walk.Hash(&x,sizeof(x));walk.Hash(&y,sizeof(y));
                if(p.key==walk.hash&&p.width==w&&p.height==h)continue;
                if(p.width!=w||p.height!=h||!p.source||!p.intermediate||!p.result) {
                    p.source=Allocate((w+1)/2,(h+1)/2);p.intermediate=Allocate((w+1)/2,(h+1)/2);p.result=Allocate(w,h);
                    p.width=w;p.height=h;
                    if(!p.source||!p.intermediate||!p.result) {PRISM_LOG_ERROR("SURFACE-EFFECT","Buffer allocation failed");wlr_scene_node_set_enabled(&p.tree->node,false);continue;}
                }
                if(r.blur_radius>0) {
                    auto* pass=wlr_renderer_begin_buffer_pass(renderer,p.source->buffer,nullptr);
                    if(!pass) {wlr_scene_node_set_enabled(&p.tree->node,false);PRISM_LOG_ERROR("SURFACE-EFFECT","Capture pass failed");continue;}
                    wlr_render_rect_options clear{};clear.box={0,0,p.source->buffer->width,p.source->buffer->height};
                    clear.blend_mode=WLR_RENDER_BLEND_MODE_NONE;wlr_render_pass_add_rect(pass,&clear);
                    Walk capture{&view->scene_tree->node,&p.tree->node,renderer,pass,double(x),double(y),.5};
                    capture.excluded=&own;capture.paint_generations=&paint_generations;capture.Node(&scene->tree.node);
                    if(!wlr_render_pass_submit(pass)||!Blur(*p.intermediate,*p.source,r.blur_radius/2,true)||!Blur(*p.source,*p.intermediate,r.blur_radius/2,false)) {
                        wlr_scene_node_set_enabled(&p.tree->node,false);PRISM_LOG_ERROR("SURFACE-EFFECT","Capture or blur submission failed");continue;
                    }
                }
                if(!Draw(*p.result,*p.source,material,w,h,&r,padding,decoration?&style:nullptr)) {
                    wlr_scene_node_set_enabled(&p.tree->node,false);PRISM_LOG_ERROR("SURFACE-EFFECT","Material pass failed");continue;
                }
                wlr_scene_buffer_set_buffer(p.node,p.result->buffer);p.key=walk.hash;
                p.generation=++generated;paint_generations[p.node]=p.generation;
                if(generated<=12)PRISM_LOG_INFO("SURFACE-EFFECT","Rendered region %dx%d blur=%.1f lower-scene-only shell=%d",w,h,r.blur_radius,view->shell_role);
            }
        }
    }
};
SurfaceEffects::SurfaceEffects(wl_display* d,wlr_renderer* r,wlr_allocator* a):impl_(std::make_unique<Impl>(d,r,a)){}
SurfaceEffects::~SurfaceEffects()=default;
bool SurfaceEffects::Supported()const{return impl_->supported;}
void SurfaceEffects::Update(wlr_scene* s,std::span<WlrXdgView* const> v,WlrXdgView* f,const contracts::ThemeSnapshot* t){impl_->Update(s,v,f,t);}
}
