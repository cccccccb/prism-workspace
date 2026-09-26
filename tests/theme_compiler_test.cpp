#include "prism/theme/compiler.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs=std::filesystem;
using namespace prism;
std::string Read(const fs::path& path) {
    std::ifstream input(path); assert(input);
    return {std::istreambuf_iterator<char>{input},{}};
}
std::string Replace(std::string source,std::string_view before,std::string_view after) {
    const auto at=source.find(before); assert(at!=std::string::npos);
    source.replace(at,before.size(),after); return source;
}
template<class F> void Reject(F callback) {
    bool rejected=false; try { callback(); } catch (const std::exception&) { rejected=true; }
    assert(rejected);
}
int main() {
    const fs::path root=fs::path(PRISM_SOURCE_ROOT)/"resources/themes";
    const auto glass=theme::LoadTheme(root,"glass",7);
    assert(glass.id=="glass" && glass.generation==7 && glass.schema_version==1);
    const auto* window=contracts::FindThemeMaterial(glass,"window"); assert(window);
    assert(window->tint==(contracts::Color{231,237,241,212}));
    assert(window->backdrop_blur==12 && window->radius==12);
    assert(glass.normal.radius==window->radius && glass.focused.radius==window->radius);
    assert(!glass.fullscreen.enabled && glass.controls.toggle_inset==2);
    for (const auto* id:{"glass","translucent","transparent","square"}) {
        auto snapshot=theme::LoadTheme(root,id,8);
        assert(snapshot.layout==glass.layout);
        assert(contracts::DecodeTheme(contracts::EncodeTheme(snapshot))==snapshot);
        const auto* material=contracts::FindThemeMaterial(snapshot,"window"); assert(material);
        assert(material->input_shape==contracts::ThemeInputShape::Bounds);
        assert(snapshot.normal.radius==material->radius && snapshot.focused.radius==material->radius);
        if (std::string_view(id)=="translucent") assert(material->backdrop_blur==0 && material->tint.a==212);
        if (std::string_view(id)=="transparent") assert(material->backdrop_blur==0 && material->tint.a==0);
        if (std::string_view(id)=="square") assert(material->radius==0 && material->backdrop_blur==0 && material->tint.a==255);
    }
    const auto source=Read(root/"glass/theme.prism");
    Reject([&]{ theme::CompileTheme(Replace(source,"schemaVersion: 1","schemaVersion: 2")); });
    Reject([&]{ theme::CompileTheme(Replace(source,"schemaVersion: 1","schemaVersion: 1, schemaVersion: 1")); });
    Reject([&]{ theme::CompileTheme(Replace(source,"blur: \"@blur_radius\"","unknownBlur: 12")); });
    Reject([&]{ theme::CompileTheme(Replace(source,"Number(\"blur_radius\", value: 12)","Number(\"blur_radius\", value: 49)")); });
    Reject([&]{ theme::CompileTheme(Replace(source,"inputShape: \"bounds\"","inputShape: \"automatic\"")); });
    Reject([&]{ theme::CompileTheme(Replace(source,"tint: \"@windowTint\"","tint: \"@font_body\"")); });
    Reject([&]{ theme::CompileTheme(Replace(source,"tint: \"@windowTint\"","tint: \"@missing\"")); });
    Reject([&]{ theme::CompileTheme(Replace(source,"shape: \"window\"","shape: \"missing\"")); });
    Reject([&]{ theme::CompileTheme(Replace(source,"shape: \"window\"","shape: \"panel\"")); });
    Reject([&]{ theme::CompileTheme(Replace(source,"Number(\"font_body\", value: 14)",
        "Number(\"font_body\", value: \"@text\")")); });
    Reject([&]{ theme::CompileTheme(Replace(Replace(source,"Number(\"font_body\", value: 14)",
        "Number(\"font_body\", value: \"@font_caption\")"),"Number(\"font_caption\", value: 11)",
        "Number(\"font_caption\", value: \"@font_body\")")); });
    auto duplicate=source; duplicate.insert(duplicate.rfind('}'),"Number(\"font_body\", value: 14)\n");
    Reject([&]{ theme::CompileTheme(duplicate); });
    auto unknown=source; unknown.insert(unknown.rfind('}'),"Unknown()\n");
    Reject([&]{ theme::CompileTheme(unknown); });
    auto alias=source; alias.insert(alias.rfind('}'),"Number(\"alias\", value: \"@font_body\")\n");
    assert(contracts::ThemeNumberValue(theme::CompileTheme(alias),"alias")==14);
    auto negative=Replace(source,"Number(\"shadow_offset_y\", value: 4)","Number(\"shadow_offset_y\", value: -4)");
    assert(theme::CompileTheme(negative).normal.shadow_y==-4);
    Reject([&]{ theme::CompileTheme(std::string(contracts::kMaxThemePayload+1,'A')); });
    Reject([&]{ theme::LoadTheme(root,"../glass"); });
    Reject([&]{ theme::LoadTheme(root,"does_not_exist"); });

    const auto temp=fs::temp_directory_path()/("prism-theme-compiler-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp/"themes/wrong"); fs::create_directories(temp/"outside");
    struct Cleanup { fs::path path; ~Cleanup(){std::error_code error;fs::remove_all(path,error);} } cleanup{temp};
    std::ofstream(temp/"themes/wrong/theme.prism")<<source;
    Reject([&]{ theme::LoadTheme(temp/"themes","wrong"); });
    std::ofstream(temp/"outside/theme.prism")<<Replace(source,"Theme(\"glass\"","Theme(\"escape\"");
    fs::create_directory_symlink(temp/"outside",temp/"themes/escape");
    Reject([&]{ theme::LoadTheme(temp/"themes","escape"); });
    assert(theme::LoadTheme(theme::DefaultThemeRoot(),"glass").id=="glass");
}
