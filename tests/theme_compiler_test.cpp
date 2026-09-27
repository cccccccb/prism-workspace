#include "prism/theme/compiler.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace prism;

std::string Read(const fs::path &path)
{
    std::ifstream input(path);
    assert(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}

std::string Replace(std::string source, std::string_view before, std::string_view after)
{
    const auto at = source.find(before);
    assert(at != std::string::npos);
    source.replace(at, before.size(), after);
    return source;
}

std::string WithoutPalettes(std::string source)
{
    for (auto at = source.find("Palette("); at != std::string::npos; at = source.find("Palette(")) {
        const auto begin = source.find('{', at);
        assert(begin != std::string::npos);
        unsigned depth = 1;
        auto end = begin + 1;
        for (; end < source.size() && depth; ++end) {
            if (source[end] == '{') {
                ++depth;
            } else if (source[end] == '}') {
                --depth;
            }
        }
        assert(!depth);
        source.erase(at, end - at);
    }
    return source;
}

template <class F> void Reject(F callback)
{
    bool rejected = false;
    try {
        callback();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}

int main()
{
    const fs::path root = fs::path(PRISM_SOURCE_ROOT) / "resources/themes";
    const auto glass = theme::LoadTheme(root, "glass", 7);
    assert(glass.id == "glass" && glass.generation == 7 && glass.schema_version == 2 &&
           glass.color_scheme == "dark");
    const auto *window = contracts::FindThemeMaterial(glass, "window");
    assert(window);
    assert(window->tint == contracts::ThemeColorValue(glass, "windowTint"));
    assert(window->tint.a > 0 && window->tint.a < 255);
    assert(window->backdrop_blur == contracts::ThemeNumberValue(glass, "blur_radius") &&
           window->radius == 12);
    assert(glass.normal.radius == window->radius && glass.focused.radius == window->radius);
    assert(!glass.fullscreen.enabled && glass.controls.toggle_inset == 2);
    for (const auto *id : {"glass", "translucent", "transparent", "square"}) {
        auto snapshot = theme::LoadTheme(root, id, 8);
        assert(snapshot.layout == glass.layout);
        assert(contracts::DecodeTheme(contracts::EncodeTheme(snapshot)) == snapshot);
        const auto *material = contracts::FindThemeMaterial(snapshot, "window");
        assert(material);
        assert(material->input_shape == contracts::ThemeInputShape::Bounds);
        assert(snapshot.normal.radius == material->radius &&
               snapshot.focused.radius == material->radius);
        if (std::string_view(id) == "translucent") {
            assert(material->backdrop_blur == 0 && material->tint.a > 0 && material->tint.a < 255);
        }
        if (std::string_view(id) == "transparent") {
            assert(material->backdrop_blur == 0 && material->tint.a == 0);
        }
        if (std::string_view(id) == "square") {
            assert(material->radius == 0 && material->backdrop_blur == 0 &&
                   material->tint.a == 255);
        }
        const auto light = theme::LoadTheme(root, id, 9, "light");
        assert(light.id == snapshot.id && light.color_scheme == "light" && light.generation == 9);
        assert(light.layout == snapshot.layout && light.numbers == snapshot.numbers);
        assert(light.normal.radius == snapshot.normal.radius);
        assert(contracts::ThemeColorValue(light, "text") !=
               contracts::ThemeColorValue(snapshot, "text"));
        assert(contracts::DecodeTheme(contracts::EncodeTheme(light)) == light);
    }
    const auto source = Read(root / "glass/theme.prism");
    Reject([&] { theme::CompileTheme(Replace(source, "schemaVersion: 2", "schemaVersion: 3")); });
    Reject([&] {
        theme::CompileTheme(
            Replace(source, "schemaVersion: 2", "schemaVersion: 2, schemaVersion: 2"));
    });
    Reject(
        [&] { theme::CompileTheme(Replace(source, "blur: \"@blur_radius\"", "unknownBlur: 12")); });
    Reject([&] {
        theme::CompileTheme(Replace(source, "Number(\"blur_radius\", value: 16)",
                                    "Number(\"blur_radius\", value: 49)"));
    });
    Reject([&] {
        theme::CompileTheme(Replace(source, "inputShape: \"bounds\"", "inputShape: \"automatic\""));
    });
    Reject([&] {
        theme::CompileTheme(Replace(source, "tint: \"@windowTint\"", "tint: \"@font_body\""));
    });
    Reject([&] {
        theme::CompileTheme(Replace(source, "tint: \"@windowTint\"", "tint: \"@missing\""));
    });
    Reject(
        [&] { theme::CompileTheme(Replace(source, "shape: \"window\"", "shape: \"missing\"")); });
    Reject([&] { theme::CompileTheme(Replace(source, "shape: \"window\"", "shape: \"panel\"")); });
    Reject([&] {
        theme::CompileTheme(Replace(source, "Number(\"font_body\", value: 14)",
                                    "Number(\"font_body\", value: \"@text\")"));
    });
    Reject([&] {
        theme::CompileTheme(Replace(Replace(source, "Number(\"font_body\", value: 14)",
                                            "Number(\"font_body\", value: \"@font_caption\")"),
                                    "Number(\"font_caption\", value: 11)",
                                    "Number(\"font_caption\", value: \"@font_body\")"));
    });
    auto duplicate = source;
    duplicate.insert(duplicate.rfind('}'), "Number(\"font_body\", value: 14)\n");
    Reject([&] { theme::CompileTheme(duplicate); });
    auto unknown = source;
    unknown.insert(unknown.rfind('}'), "Unknown()\n");
    Reject([&] { theme::CompileTheme(unknown); });
    auto alias = source;
    alias.insert(alias.rfind('}'), "Number(\"alias\", value: \"@font_body\")\n");
    assert(contracts::ThemeNumberValue(theme::CompileTheme(alias), "alias") == 14);

    // A schema-1 package keeps its original dark representation and cannot
    // silently substitute it for a requested light palette.
    const auto base = WithoutPalettes(source);
    const auto legacy = Replace(base, "schemaVersion: 2", "schemaVersion: 1");
    const auto old = theme::CompileTheme(legacy, 5);
    assert(old.schema_version == 1 && old.color_scheme == "dark");
    assert(contracts::DecodeTheme(contracts::EncodeTheme(old)) == old);
    Reject([&] { theme::CompileTheme(legacy, 5, "light"); });
    Reject([&] { theme::CompileTheme(Replace(source, "schemaVersion: 2", "schemaVersion: 1")); });
    Reject([&] { theme::CompileTheme(base, 5, "light"); });
    Reject([&] { theme::CompileTheme(source, 5, "auto"); });

    auto palette_source = Replace(base, "tint: \"@windowTint\"", "tint: \"@windowAlias\"");
    palette_source.insert(palette_source.rfind('}'), R"(
        Color("windowAlias",value:"@windowTint")
        Number("radiusAlias",value:"@window_radius")
        Number("fontAlias",value:"@font_body")
        Palette("light") {
            Color("windowTint",value:#F4F7FCAA)
            Color("border",value:#233548FF)
            Color("toggleKnob",value:#111111FF)
            Number("window_radius",value:0)
            Number("font_body",value:16)
        }
    )");
    const auto dark_alias = theme::CompileTheme(palette_source, 10);
    const auto light_alias = theme::CompileTheme(palette_source, 11, "light");
    const contracts::Color light_window{244, 247, 252, 170};
    assert(contracts::FindThemeMaterial(light_alias, "window")->tint == light_window);
    assert(contracts::ThemeColorValue(light_alias, "windowAlias") == light_window);
    assert(contracts::ThemeNumberValue(light_alias, "fontAlias") == 16);
    assert(contracts::ThemeNumberValue(light_alias, "radiusAlias") == 0);
    assert(light_alias.normal.radius == 0 && light_alias.focused.radius == 0);
    assert(light_alias.normal.border == contracts::ThemeColorValue(light_alias, "border"));
    assert(light_alias.controls.toggle_knob ==
           contracts::ThemeColorValue(light_alias, "toggleKnob"));
    assert(dark_alias.normal.radius == 12 &&
           contracts::ThemeNumberValue(dark_alias, "fontAlias") == 14);
    // Overrides are checked even if their palette is not selected.
    const auto invalid_palette = [&](std::string_view declaration) {
        auto candidate = base;
        candidate.insert(candidate.rfind('}'), declaration);
        Reject([&] { theme::CompileTheme(candidate); });
    };
    invalid_palette("Palette(\"auto\") {}\n");
    invalid_palette("Palette(\"light\") { Color(\"missing\",value:#FFFFFFFF) }\n");
    invalid_palette("Palette(\"light\") { Number(\"text\",value:2) }\n");
    invalid_palette(
        "Palette(\"light\") { Color(\"text\",value:#FFFFFFFF) Color(\"text\",value:#000000FF) }\n");
    invalid_palette("Palette(\"light\") {} Palette(\"light\") {}\n");
    invalid_palette("Palette(\"light\") { Palette(\"dark\") {} }\n");
    invalid_palette("Palette(\"light\") { Color(\"text\",value:#FFFFFFFF) { "
                    "Color(\"text\",value:#000000FF) } }\n");
    invalid_palette("Palette(\"light\") { Color(\"text\",value:\"@missing\") }\n");
    invalid_palette("Palette(\"light\") { Number(\"font_body\",value:20000) }\n");
    auto explicit_dark = base;
    explicit_dark.insert(explicit_dark.rfind('}'),
                         "Palette(\"dark\") { Number(\"font_body\",value:15) }\n");
    assert(contracts::ThemeNumberValue(theme::CompileTheme(explicit_dark), "font_body") == 15);
    auto negative = Replace(source, "Number(\"shadow_offset_y\", value: 4)",
                            "Number(\"shadow_offset_y\", value: -4)");
    assert(theme::CompileTheme(negative).normal.shadow_y == -4);
    Reject([&] { theme::CompileTheme(std::string(contracts::kMaxThemePayload + 1, 'A')); });
    Reject([&] { theme::LoadTheme(root, "../glass"); });
    Reject([&] { theme::LoadTheme(root, "does_not_exist"); });

    const auto temp = fs::temp_directory_path() /
                      ("prism-theme-compiler-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp / "themes/wrong");
    fs::create_directories(temp / "outside");

    struct Cleanup {
        fs::path path;

        ~Cleanup()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{temp};

    std::ofstream(temp / "themes/wrong/theme.prism") << source;
    Reject([&] { theme::LoadTheme(temp / "themes", "wrong"); });
    std::ofstream(temp / "outside/theme.prism")
        << Replace(source, "Theme(\"glass\"", "Theme(\"escape\"");
    fs::create_directory_symlink(temp / "outside", temp / "themes/escape");
    Reject([&] { theme::LoadTheme(temp / "themes", "escape"); });
    assert(theme::LoadTheme(theme::DefaultThemeRoot(), "glass").id == "glass");
}
