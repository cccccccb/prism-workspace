#pragma once
#include "prism/contracts/display_list.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace prism::contracts {
inline constexpr std::size_t kMaxThemePayload = 65536;
struct ThemeNumber { std::string name; double value{}; bool operator==(const ThemeNumber&) const = default; };
struct ThemeColor { std::string name; Color value{}; bool operator==(const ThemeColor&) const = default; };
enum class ThemeInputShape : std::uint8_t { Visible, Bounds };
struct ThemeMaterial {
    std::string name;
    Color tint{};
    double radius{}, backdrop_blur{}, border_width{};
    Color border{};
    double shadow_blur{}, shadow_y{};
    Color shadow{};
    double inner_shadow_blur{}, inner_shadow_y{};
    Color inner_shadow{};
    ThemeInputShape input_shape{ThemeInputShape::Bounds};
    bool operator==(const ThemeMaterial&) const = default;
};
struct ThemeDecoration {
    bool enabled{true};
    double radius{}, border_width{};
    Color border{};
    double shadow_blur{}, shadow_y{};
    Color shadow{};
    bool operator==(const ThemeDecoration&) const = default;
};
struct ThemeLayout {
    double topbar_surface_height{}, dock_surface_height{}, dock_max_width{1}, outer_gap{}, inner_gap{};
    bool operator==(const ThemeLayout&) const = default;
};
struct ThemeControls {
    Color hover{}, focus{}, toggle_knob{};
    double focus_width{}, toggle_inset{}, toggle_knob_radius{}, toggle_track_radius{}, inner_shadow_y{};
    bool operator==(const ThemeControls&) const = default;
};
// A resolved value object. It contains no DSL syntax, widget tree or backend handles.
struct ThemeSnapshot {
    std::uint32_t schema_version{1};
    std::uint64_t generation{};
    std::string id, name;
    std::vector<ThemeNumber> numbers;
    std::vector<ThemeColor> colors;
    std::vector<ThemeMaterial> materials;
    ThemeLayout layout;
    ThemeDecoration normal, focused, fullscreen;
    ThemeControls controls;
    bool operator==(const ThemeSnapshot&) const = default;
};
std::optional<double> ThemeNumberValue(const ThemeSnapshot&, std::string_view);
std::optional<Color> ThemeColorValue(const ThemeSnapshot&, std::string_view);
const ThemeMaterial* FindThemeMaterial(const ThemeSnapshot&, std::string_view);
// Throws invalid_argument before any state mutation on an invalid or oversized snapshot.
void ValidateTheme(const ThemeSnapshot&);
std::vector<std::uint8_t> EncodeTheme(const ThemeSnapshot&);
ThemeSnapshot DecodeTheme(std::span<const std::uint8_t>);
struct ThemeRequest { std::uint64_t request{}; std::string id; }; // empty id queries the current theme
// Received by clients after the WM and every participating host have acknowledged installation.
enum class ThemeStatus : std::uint8_t { Current, Applied, Rejected };
struct ThemeEvent {
    std::uint64_t request{}, generation{};
    ThemeStatus status{ThemeStatus::Current};
    std::string id, name, detail;
};
struct ThemeApplied { std::uint64_t generation{}; bool success{}; std::string detail; };
std::vector<std::uint8_t> EncodeThemeRequest(const ThemeRequest&);
ThemeRequest DecodeThemeRequest(std::span<const std::uint8_t>);
std::vector<std::uint8_t> EncodeThemeEvent(const ThemeEvent&);
ThemeEvent DecodeThemeEvent(std::span<const std::uint8_t>);
std::vector<std::uint8_t> EncodeThemeApplied(const ThemeApplied&);
ThemeApplied DecodeThemeApplied(std::span<const std::uint8_t>);
}
