#include "prism/theme/compiler.hpp"
#include "prism/runtime/dsl_syntax.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>

namespace prism::theme {
namespace {
using runtime::SyntaxNode;
using runtime::SyntaxValue;
using contracts::Color;
[[noreturn]] void Error(int line,std::string message) {
    throw std::invalid_argument("Theme DSL line "+std::to_string(line)+": "+std::move(message));
}
bool Name(std::string_view name) {
    if (name.empty() || name.size()>64) return false;
    for (unsigned char c:name) if (!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
        (c>='0' && c<='9') || c=='_' || c=='-')) return false;
    return true;
}
bool PackageId(std::string_view id) {
    return Name(id) && id.size()<=64 && id.find('.')==std::string_view::npos;
}
struct Args {
    const SyntaxNode& node;
    std::map<std::string,const SyntaxValue*> values;
    Args(const SyntaxNode& source,std::initializer_list<std::string_view> allowed,
         bool positional=false,bool children=false):node(source) {
        if (!source.modifiers.empty()) Error(source.line,"theme components cannot have modifiers");
        if (!children && !source.children.empty()) Error(source.line,"unexpected children of "+source.name);
        for (const auto& arg:source.arguments) {
            const auto key=arg.name.empty()?std::string("$name"):arg.name;
            if ((arg.name.empty() && !positional) || (!arg.name.empty() &&
                std::find(allowed.begin(),allowed.end(),arg.name)==allowed.end()))
                Error(arg.line,"unknown argument of "+source.name+": "+key);
            if (!values.emplace(key,&arg.value).second) Error(arg.line,"duplicate argument: "+key);
        }
    }
    const SyntaxValue& Get(std::string_view key) const {
        auto it=values.find(std::string(key));
        if (it==values.end()) Error(node.line,"missing "+std::string(key)+" on "+node.name);
        return *it->second;
    }
    std::string Text(std::string_view key) const {
        const auto* value=std::get_if<std::string>(&Get(key).data);
        if (!value || value->empty() || value->size()>256 || value->find('\0')!=std::string::npos)
            Error(node.line,"expected a nonempty string for "+std::string(key));
        return *value;
    }
    bool Boolean(std::string_view key) const {
        if (const auto* value=std::get_if<bool>(&Get(key).data)) return *value;
        Error(node.line,"expected boolean for "+std::string(key));
    }
};
using TokenValue=std::variant<double,Color>;
struct Token {
    const SyntaxValue* source{};
    int line{};
    bool color{};
    unsigned visiting{};
    std::optional<TokenValue> resolved;
};
class Compiler {
    std::map<std::string,Token> tokens;
    TokenValue Resolve(Token& token,unsigned depth=0) {
        if (token.resolved) return *token.resolved;
        if (token.visiting || depth>64) Error(token.line,"cyclic or excessively deep token references");
        token.visiting=1;
        TokenValue result;
        if (const auto* reference=std::get_if<std::string>(&token.source->data)) {
            if (reference->size()<2 || reference->front()!='@') Error(token.line,"token value must be typed literal or @reference");
            auto it=tokens.find(reference->substr(1));
            if (it==tokens.end()) Error(token.line,"unknown token: "+*reference);
            if (token.color!=it->second.color) Error(token.line,"token reference has wrong type: "+*reference);
            result=Resolve(it->second,depth+1);
        } else if (token.color) {
            const auto* value=std::get_if<runtime::ColorValue>(&token.source->data);
            if (!value) Error(token.line,"expected color token value");
            result=Color{static_cast<std::uint8_t>(value->rgba>>24),static_cast<std::uint8_t>(value->rgba>>16),
                static_cast<std::uint8_t>(value->rgba>>8),static_cast<std::uint8_t>(value->rgba)};
        } else {
            const auto* value=std::get_if<double>(&token.source->data);
            if (!value || !std::isfinite(*value)) Error(token.line,"expected finite number token value");
            result=*value;
        }
        token.visiting=0; token.resolved=result; return result;
    }
    std::optional<TokenValue> Reference(const SyntaxValue& source,int line) {
        const auto* name=std::get_if<std::string>(&source.data);
        if (!name) return std::nullopt;
        if (name->size()<2 || name->front()!='@') Error(line,"expected typed literal or @token");
        auto it=tokens.find(name->substr(1));
        if (it==tokens.end()) Error(line,"unknown token: "+*name);
        return Resolve(it->second);
    }
    double Number(const Args& args,std::string_view key,double min,double max) {
        const auto& source=args.Get(key);
        auto reference=Reference(source,args.node.line);
        const auto* value=reference?std::get_if<double>(&*reference):std::get_if<double>(&source.data);
        if (!value || !std::isfinite(*value) || *value<min || *value>max)
            Error(args.node.line,"invalid number/type/range for "+std::string(key));
        return *value;
    }
    Color ColorValue(const Args& args,std::string_view key) {
        const auto& source=args.Get(key);
        if (auto reference=Reference(source,args.node.line)) {
            if (const auto* value=std::get_if<Color>(&*reference)) return *value;
            Error(args.node.line,"expected color token for "+std::string(key));
        }
        if (const auto* value=std::get_if<runtime::ColorValue>(&source.data))
            return {static_cast<std::uint8_t>(value->rgba>>24),static_cast<std::uint8_t>(value->rgba>>16),
                static_cast<std::uint8_t>(value->rgba>>8),static_cast<std::uint8_t>(value->rgba)};
        Error(args.node.line,"expected color for "+std::string(key));
    }
    contracts::ThemeMaterial Material(const SyntaxNode& node) {
        Args args(node,{"tint","radius","blur","borderWidth","border","shadowBlur","shadowY","shadow",
            "innerShadowBlur","innerShadowY","innerShadow","inputShape"},true);
        contracts::ThemeMaterial out;
        out.name=args.Text("$name"); if (!Name(out.name)) Error(node.line,"invalid material name");
        out.tint=ColorValue(args,"tint"); out.radius=Number(args,"radius",0,256);
        out.backdrop_blur=Number(args,"blur",0,48); out.border_width=Number(args,"borderWidth",0,32);
        out.border=ColorValue(args,"border"); out.shadow_blur=Number(args,"shadowBlur",0,128);
        out.shadow_y=Number(args,"shadowY",-128,128); out.shadow=ColorValue(args,"shadow");
        out.inner_shadow_blur=Number(args,"innerShadowBlur",0,128);
        out.inner_shadow_y=Number(args,"innerShadowY",-128,128); out.inner_shadow=ColorValue(args,"innerShadow");
        const auto input=args.Text("inputShape");
        if (input=="bounds") out.input_shape=contracts::ThemeInputShape::Bounds;
        else if (input=="visible") out.input_shape=contracts::ThemeInputShape::Visible;
        else Error(node.line,"inputShape must be bounds or visible");
        return out;
    }
public:
    contracts::ThemeSnapshot Compile(const SyntaxNode& root,std::uint64_t generation,std::string_view color_scheme) {
        if(color_scheme!="dark" && color_scheme!="light") Error(root.line,"unsupported color scheme");
        if (root.name!="Theme") Error(root.line,"root must be Theme");
        Args args(root,{"name","schemaVersion"},true,true);
        contracts::ThemeSnapshot out;
        out.id=args.Text("$name"); if (!PackageId(out.id)) Error(root.line,"invalid theme ID");
        out.name=args.Text("name"); out.generation=generation;
        const auto* version=std::get_if<double>(&args.Get("schemaVersion").data);
        if (!version || (*version!=1 && *version!=2)) Error(root.line,"unsupported theme schema version");
        out.schema_version=static_cast<std::uint32_t>(*version);
        out.color_scheme=color_scheme;
        if(out.schema_version==1 && color_scheme!="dark") Error(root.line,"schema 1 only supports dark");
        if (root.children.size()>512) Error(root.line,"excessive theme components");
        for (const auto& node:root.children) if (node.name=="Number" || node.name=="Color") {
            Args token(node,{"value"},true);
            const auto name=token.Text("$name");
            if (!Name(name)) Error(node.line,"invalid token name");
            if (!tokens.emplace(name,Token{&token.Get("value"),node.line,node.name=="Color",0,{}}).second)
                Error(node.line,"duplicate token: "+name);
        }
        // Keep references unresolved until the selected palette is applied.
        // Aliases and material fields then resolve against the same token map.
        const auto base=tokens;
        std::map<std::string,std::map<std::string,Token>> palettes;
        for(const auto& node:root.children) if(node.name=="Palette") {
            if(out.schema_version!=2) Error(node.line,"Palette requires schema 2");
            Args palette(node,{},true,true);
            const auto scheme=palette.Text("$name");
            if(scheme!="dark" && scheme!="light") Error(node.line,"unsupported palette color scheme");
            if(palettes.contains(scheme)) Error(node.line,"duplicate Palette: "+scheme);
            if(node.children.size()>128) Error(node.line,"excessive palette overrides");
            auto& overrides=palettes[scheme];
            for(const auto& child:node.children) {
                if(child.name!="Color" && child.name!="Number") Error(child.line,"Palette only contains Color or Number overrides");
                Args token(child,{"value"},true);
                const auto name=token.Text("$name");
                const auto existing=base.find(name);
                if(existing==base.end()) Error(child.line,"unknown palette override: "+name);
                if(existing->second.color!=(child.name=="Color")) Error(child.line,"palette override has wrong token type: "+name);
                if(!overrides.emplace(name,Token{&token.Get("value"),child.line,child.name=="Color",0,{}}).second)
                    Error(child.line,"duplicate palette override: "+name);
            }
        }
        if(color_scheme=="light" && !palettes.contains("light")) Error(root.line,"theme has no light Palette");
        const auto validate_tokens=[&] {
            for(auto& [name,token]:tokens) {
                const auto value=Resolve(token);
                if(!token.color && (std::get<double>(value)<-16384 || std::get<double>(value)>16384))
                    Error(token.line,"number token outside supported range: "+name);
            }
        };
        // Reject invalid declarations in unselected palettes too. Each palette
        // starts from the unresolved base, preventing cache leakage across schemes.
        validate_tokens();
        for(const auto& [scheme,overrides]:palettes) {
            tokens=base;
            for(const auto& [name,token]:overrides)tokens[name]=token;
            validate_tokens();
        }
        tokens=base;
        if(const auto selected=palettes.find(std::string(color_scheme));selected!=palettes.end())
            for(const auto& [name,token]:selected->second)tokens[name]=token;
        for (auto& [name,token]:tokens) {
            auto value=Resolve(token);
            if (token.color) out.colors.push_back({name,std::get<Color>(value)});
            else out.numbers.push_back({name,std::get<double>(value)});
        }
        bool layout=false,controls=false;
        std::set<std::string> materials,decorations;
        for (const auto& node:root.children) {
            if (node.name=="Number" || node.name=="Color" || node.name=="Palette") continue;
            if (node.name=="Material") {
                auto material=Material(node);
                if (!materials.insert(material.name).second) Error(node.line,"duplicate material: "+material.name);
                out.materials.push_back(std::move(material));
            } else if (node.name=="Layout") {
                if (layout) Error(node.line,"duplicate Layout");
                layout=true;
                Args item(node,{"topbarSurfaceHeight","dockSurfaceHeight","dockMaxWidth","outerGap","innerGap"});
                out.layout={Number(item,"topbarSurfaceHeight",0,4096),Number(item,"dockSurfaceHeight",0,4096),
                    Number(item,"dockMaxWidth",1,8192),Number(item,"outerGap",0,256),Number(item,"innerGap",0,256)};
            } else if (node.name=="Controls") {
                if (controls) Error(node.line,"duplicate Controls");
                controls=true;
                Args item(node,{"hover","focus","toggleKnob","focusWidth","toggleInset","toggleKnobRadius",
                    "toggleTrackRadius","innerShadowY"});
                out.controls={ColorValue(item,"hover"),ColorValue(item,"focus"),ColorValue(item,"toggleKnob"),
                    Number(item,"focusWidth",0,32),Number(item,"toggleInset",0,64),
                    Number(item,"toggleKnobRadius",0,256),Number(item,"toggleTrackRadius",0,256),
                    Number(item,"innerShadowY",-128,128)};
            } else if (node.name!="Decoration") Error(node.line,"unknown theme component: "+node.name);
        }
        for (const auto& node:root.children) if (node.name=="Decoration") {
            Args item(node,{"shape","enabled","borderWidth","border","shadowBlur","shadowY","shadow"},true);
            const auto state=item.Text("$name"),shape=item.Text("shape");
            if (shape!="window") Error(node.line,"v1 Decoration shape must be window");
            if (!decorations.insert(state).second) Error(node.line,"duplicate Decoration: "+state);
            auto material=std::find_if(out.materials.begin(),out.materials.end(),[&](const auto& m){return m.name==shape;});
            if (material==out.materials.end()) Error(node.line,"unknown decoration shape material: "+shape);
            contracts::ThemeDecoration decoration{item.Boolean("enabled"),material->radius,
                Number(item,"borderWidth",0,32),ColorValue(item,"border"),
                Number(item,"shadowBlur",0,128),Number(item,"shadowY",-128,128),ColorValue(item,"shadow")};
            if (state=="normal") out.normal=decoration;
            else if (state=="focused") out.focused=decoration;
            else if (state=="fullscreen") out.fullscreen=decoration;
            else Error(node.line,"unknown decoration state: "+state);
        }
        if (!layout || !controls || decorations.size()!=3) Error(root.line,"theme needs Layout, Controls and three Decoration states");
        for (const auto* name:{"window","panel","card","control"})
            if (!materials.contains(name)) Error(root.line,"missing semantic material: "+std::string(name));
        contracts::ValidateTheme(out);
        return out;
    }
};
} // namespace

contracts::ThemeSnapshot CompileTheme(std::string_view source,std::uint64_t generation,std::string_view color_scheme) {
    if (source.empty() || source.size()>contracts::kMaxThemePayload || source.find('\0')!=std::string_view::npos)
        throw std::invalid_argument("Theme source is empty, oversized or contains NUL");
    return Compiler{}.Compile(runtime::ParseSyntax(source),generation,color_scheme);
}
contracts::ThemeSnapshot LoadTheme(const std::filesystem::path& root,std::string_view id,std::uint64_t generation,std::string_view color_scheme) {
    if (!PackageId(id)) throw std::invalid_argument("Invalid theme package ID");
    const auto base=std::filesystem::canonical(root);
    const auto file=std::filesystem::canonical(base/std::string(id)/"theme.prism");
    auto a=base.begin(),b=file.begin();
    for (;a!=base.end() && b!=file.end() && *a==*b;++a,++b) {}
    if (a!=base.end() || b==file.end()) throw std::invalid_argument("Theme package escapes its root");
    const auto size=std::filesystem::file_size(file);
    if (!size || size>contracts::kMaxThemePayload) throw std::invalid_argument("Theme package size exceeds limit");
    std::ifstream input(file,std::ios::binary);
    std::string source(size,'\0');
    if (!input.read(source.data(),source.size())) throw std::invalid_argument("Cannot read theme package");
    auto snapshot=CompileTheme(source,generation,color_scheme);
    if (snapshot.id!=id) throw std::invalid_argument("Theme package ID does not match its directory");
    return snapshot;
}
std::filesystem::path DefaultThemeRoot() {
    const auto executable=std::filesystem::canonical("/proc/self/exe");
    return executable.parent_path().parent_path()/"share/prism/themes";
}
} // namespace prism::theme
