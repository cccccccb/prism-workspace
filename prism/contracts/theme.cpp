#include "prism/contracts/theme.hpp"
#include <bit>
#include <cmath>
#include <set>
#include <stdexcept>

namespace prism::contracts {
namespace {
constexpr std::size_t kMaxThemeTokens = 256; // Combined number and color tokens.

void Check(bool ok, const char *message)
{
    if (!ok) {
        throw std::invalid_argument(message);
    }
}

bool ColorScheme(std::string_view s)
{
    return s == "dark" || s == "light";
}

bool Identifier(std::string_view s)
{
    if (s.empty() || s.size() > 64) {
        return false;
    }
    for (auto c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool Utf8(std::string_view s)
{
    for (std::size_t at = 0; at < s.size();) {
        auto c = static_cast<unsigned char>(s[at++]);
        if (!c) {
            return false;
        }
        if (c < 128) {
            continue;
        }
        unsigned n{};
        std::uint32_t cp{}, min{};
        if ((c & 0xe0) == 0xc0) {
            n = 1;
            cp = c & 31;
            min = 128;
        } else if ((c & 0xf0) == 0xe0) {
            n = 2;
            cp = c & 15;
            min = 2048;
        } else if ((c & 0xf8) == 0xf0) {
            n = 3;
            cp = c & 7;
            min = 65536;
        } else {
            return false;
        }
        if (n > s.size() - at) {
            return false;
        }
        while (n--) {
            c = static_cast<unsigned char>(s[at++]);
            if ((c & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (c & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

void Range(double v, double lo, double hi)
{
    Check(std::isfinite(v) && v >= lo && v <= hi, "Theme number outside supported range");
}

void Decoration(const ThemeDecoration &d)
{
    Range(d.radius, 0, 256);
    Range(d.border_width, 0, 32);
    Range(d.shadow_blur, 0, 128);
    Range(d.shadow_y, -128, 128);
}

struct Writer {
    std::vector<std::uint8_t> bytes;

    void U(std::uint64_t v, unsigned n)
    {
        for (unsigned i = n; i; --i) {
            bytes.push_back(v >> ((i - 1) * 8));
        }
    }

    void Number(double v)
    {
        U(std::bit_cast<std::uint64_t>(v), 8);
    }

    void Text(std::string_view s)
    {
        Check(s.size() <= 2048 && Utf8(s), "Invalid theme text");
        U(s.size(), 2);
        bytes.insert(bytes.end(), s.begin(), s.end());
    }

    void ColorValue(Color c)
    {
        U(c.r, 1);
        U(c.g, 1);
        U(c.b, 1);
        U(c.a, 1);
    }

    void Deco(const ThemeDecoration &d)
    {
        U(d.enabled, 1);
        Number(d.radius);
        Number(d.border_width);
        ColorValue(d.border);
        Number(d.shadow_blur);
        Number(d.shadow_y);
        ColorValue(d.shadow);
    }
};

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};

    std::uint64_t U(unsigned n)
    {
        Check(n <= bytes.size() - at, "Short theme payload");
        std::uint64_t v = 0;
        while (n--) {
            v = (v << 8) | bytes[at++];
        }
        return v;
    }

    double Number()
    {
        return std::bit_cast<double>(U(8));
    }

    bool Bool()
    {
        auto v = U(1);
        Check(v <= 1, "Invalid theme Boolean");
        return v;
    }

    std::string Text()
    {
        auto n = U(2);
        Check(n <= 2048 && n <= bytes.size() - at, "Invalid theme text size");
        std::string s(reinterpret_cast<const char *>(bytes.data() + at), n);
        at += n;
        Check(Utf8(s), "Invalid theme UTF-8");
        return s;
    }

    std::string Scheme()
    {
        auto n = U(2);
        Check(n <= 5 && n <= bytes.size() - at, "Invalid color scheme size");
        std::string s(reinterpret_cast<const char *>(bytes.data() + at), n);
        at += n;
        return s;
    }

    Color ColorValue()
    {
        Color c;
        c.r = U(1);
        c.g = U(1);
        c.b = U(1);
        c.a = U(1);
        return c;
    }

    ThemeDecoration Deco()
    {
        ThemeDecoration d;
        d.enabled = Bool();
        d.radius = Number();
        d.border_width = Number();
        d.border = ColorValue();
        d.shadow_blur = Number();
        d.shadow_y = Number();
        d.shadow = ColorValue();
        return d;
    }

    void Done()
    {
        Check(at == bytes.size(), "Trailing theme payload");
    }
};

void CheckRequest(const ThemeRequest &r)
{
    Check(r.request && (r.id.empty() || Identifier(r.id)) &&
              (r.color_scheme.empty() || ColorScheme(r.color_scheme)),
          "Invalid theme request");
}

void CheckEvent(const ThemeEvent &e)
{
    Check(e.generation && Identifier(e.id) && !e.name.empty() && e.name.size() <= 256 &&
              Utf8(e.name) && static_cast<unsigned>(e.status) <= 2 && ColorScheme(e.color_scheme),
          "Invalid theme event");
    Check(e.detail.size() <= 2048 && Utf8(e.detail), "Invalid theme detail");
}

void SchemeExtension(Writer &w, std::string_view scheme)
{
    w.U(1, 1);
    w.Text(scheme);
}

std::string SchemeExtension(Reader &r)
{
    Check(r.U(1) == 1, "Unsupported theme selector extension");
    return r.Scheme();
}
} // namespace

std::optional<double> ThemeNumberValue(const ThemeSnapshot &t, std::string_view n)
{
    for (auto &v : t.numbers) {
        if (v.name == n) {
            return v.value;
        }
    }
    return {};
}

std::optional<Color> ThemeColorValue(const ThemeSnapshot &t, std::string_view n)
{
    for (auto &v : t.colors) {
        if (v.name == n) {
            return v.value;
        }
    }
    return {};
}

const ThemeMaterial *FindThemeMaterial(const ThemeSnapshot &t, std::string_view n)
{
    for (auto &v : t.materials) {
        if (v.name == n) {
            return &v;
        }
    }
    return nullptr;
}

void ValidateTheme(const ThemeSnapshot &t)
{
    Check((t.schema_version >= 1 && t.schema_version <= 3) && Identifier(t.id) && !t.name.empty() &&
              t.name.size() <= 256 && Utf8(t.name),
          "Invalid theme identity or schema");
    Check(ColorScheme(t.color_scheme) && (t.schema_version != 1 || t.color_scheme == "dark"),
          "Unsupported theme color scheme or schema");
    if (t.schema_version >= 3) {
        ValidateMotion(t.motion);
    } else {
        Check(t.motion == MotionSet{}, "Legacy themes cannot carry motion");
    }
    Check(t.numbers.size() <= kMaxThemeTokens &&
              t.colors.size() <= kMaxThemeTokens - t.numbers.size() && t.materials.size() <= 32,
          "Theme exceeds item limits");
    std::set<std::string> names;
    for (auto &v : t.numbers) {
        Check(Identifier(v.name) && names.insert(v.name).second,
              "Duplicate or invalid theme token");
        Range(v.value, -16384, 16384);
    }
    for (auto &v : t.colors) {
        Check(Identifier(v.name) && names.insert(v.name).second,
              "Duplicate or invalid theme token");
    }
    names.clear();
    for (auto &m : t.materials) {
        Check(Identifier(m.name) && names.insert(m.name).second, "Duplicate or invalid material");
        Range(m.radius, 0, 256);
        Range(m.backdrop_blur, 0, 48);
        Range(m.border_width, 0, 32);
        Range(m.shadow_blur, 0, 128);
        Range(m.shadow_y, -128, 128);
        Range(m.inner_shadow_blur, 0, 128);
        Range(m.inner_shadow_y, -128, 128);
        Check(static_cast<unsigned>(m.input_shape) <= 1, "Invalid material input shape");
    }
    Range(t.layout.topbar_surface_height, 0, 4096);
    Range(t.layout.dock_surface_height, 0, 4096);
    Range(t.layout.dock_max_width, 1, 8192);
    Range(t.layout.outer_gap, 0, 256);
    Range(t.layout.inner_gap, 0, 256);
    Decoration(t.normal);
    Decoration(t.focused);
    Decoration(t.fullscreen);
    const auto &c = t.controls;
    Range(c.focus_width, 0, 32);
    Range(c.toggle_inset, 0, 64);
    Range(c.toggle_knob_radius, 0, 256);
    Range(c.toggle_track_radius, 0, 256);
    Range(c.inner_shadow_y, -128, 128);
}

std::vector<std::uint8_t> EncodeTheme(const ThemeSnapshot &t)
{
    ValidateTheme(t);
    Writer w;
    w.U(t.schema_version, 4);
    w.U(t.generation, 8);
    w.Text(t.id);
    w.Text(t.name);
    w.U(t.numbers.size(), 2);
    for (auto &v : t.numbers) {
        w.Text(v.name);
        w.Number(v.value);
    }
    w.U(t.colors.size(), 2);
    for (auto &v : t.colors) {
        w.Text(v.name);
        w.ColorValue(v.value);
    }
    w.U(t.materials.size(), 2);
    for (auto &m : t.materials) {
        w.Text(m.name);
        w.ColorValue(m.tint);
        w.Number(m.radius);
        w.Number(m.backdrop_blur);
        w.Number(m.border_width);
        w.ColorValue(m.border);
        w.Number(m.shadow_blur);
        w.Number(m.shadow_y);
        w.ColorValue(m.shadow);
        w.Number(m.inner_shadow_blur);
        w.Number(m.inner_shadow_y);
        w.ColorValue(m.inner_shadow);
        w.U(static_cast<unsigned>(m.input_shape), 1);
    }
    const auto &l = t.layout;
    w.Number(l.topbar_surface_height);
    w.Number(l.dock_surface_height);
    w.Number(l.dock_max_width);
    w.Number(l.outer_gap);
    w.Number(l.inner_gap);
    w.Deco(t.normal);
    w.Deco(t.focused);
    w.Deco(t.fullscreen);
    const auto &c = t.controls;
    w.ColorValue(c.hover);
    w.ColorValue(c.focus);
    w.ColorValue(c.toggle_knob);
    w.Number(c.focus_width);
    w.Number(c.toggle_inset);
    w.Number(c.toggle_knob_radius);
    w.Number(c.toggle_track_radius);
    w.Number(c.inner_shadow_y);
    if (t.schema_version >= 2) {
        w.Text(t.color_scheme);
    }
    if (t.schema_version >= 3) {
        w.Text(t.motion.id);
        w.U(t.motion.transitions.size(), 2);
        for (const auto &entry : t.motion.transitions) {
            w.Text(entry.name);
            w.U(entry.duration_ms, 4);
            w.U(static_cast<unsigned>(entry.easing), 1);
        }
    }
    Check(w.bytes.size() <= kMaxThemePayload, "Theme payload too large");
    return w.bytes;
}

ThemeSnapshot DecodeTheme(std::span<const std::uint8_t> bytes)
{
    Check(bytes.size() <= kMaxThemePayload, "Theme payload too large");
    Reader r{bytes};
    ThemeSnapshot t;
    t.schema_version = r.U(4);
    t.generation = r.U(8);
    t.id = r.Text();
    t.name = r.Text();
    auto n = r.U(2);
    Check(n <= kMaxThemeTokens, "Too many theme numbers");
    while (n--) {
        t.numbers.push_back({r.Text(), r.Number()});
    }
    n = r.U(2);
    Check(n <= kMaxThemeTokens - t.numbers.size(), "Too many theme colors");
    while (n--) {
        auto name = r.Text();
        t.colors.push_back({std::move(name), r.ColorValue()});
    }
    n = r.U(2);
    Check(n <= 32, "Too many theme materials");
    while (n--) {
        ThemeMaterial m;
        m.name = r.Text();
        m.tint = r.ColorValue();
        m.radius = r.Number();
        m.backdrop_blur = r.Number();
        m.border_width = r.Number();
        m.border = r.ColorValue();
        m.shadow_blur = r.Number();
        m.shadow_y = r.Number();
        m.shadow = r.ColorValue();
        m.inner_shadow_blur = r.Number();
        m.inner_shadow_y = r.Number();
        m.inner_shadow = r.ColorValue();
        m.input_shape = static_cast<ThemeInputShape>(r.U(1));
        t.materials.push_back(std::move(m));
    }
    auto &l = t.layout;
    l.topbar_surface_height = r.Number();
    l.dock_surface_height = r.Number();
    l.dock_max_width = r.Number();
    l.outer_gap = r.Number();
    l.inner_gap = r.Number();
    t.normal = r.Deco();
    t.focused = r.Deco();
    t.fullscreen = r.Deco();
    auto &c = t.controls;
    c.hover = r.ColorValue();
    c.focus = r.ColorValue();
    c.toggle_knob = r.ColorValue();
    c.focus_width = r.Number();
    c.toggle_inset = r.Number();
    c.toggle_knob_radius = r.Number();
    c.toggle_track_radius = r.Number();
    c.inner_shadow_y = r.Number();
    if (t.schema_version >= 2) {
        t.color_scheme = r.Scheme();
    }
    if (t.schema_version >= 3) {
        t.motion.id = r.Text();
        auto count = r.U(2);
        Check(count <= 64, "Too many motion transitions");
        while (count--) {
            t.motion.transitions.push_back(
                {r.Text(), static_cast<std::uint32_t>(r.U(4)), static_cast<MotionEasing>(r.U(1))});
        }
    }
    r.Done();
    ValidateTheme(t);
    return t;
}

std::vector<std::uint8_t> EncodeThemeRequest(const ThemeRequest &v)
{
    CheckRequest(v);
    Writer w;
    w.U(v.request, 8);
    w.Text(v.id);
    if (!v.color_scheme.empty()) {
        SchemeExtension(w, v.color_scheme);
    }
    return w.bytes;
}

ThemeRequest DecodeThemeRequest(std::span<const std::uint8_t> b)
{
    Reader r{b};
    ThemeRequest v;
    v.request = r.U(8);
    v.id = r.Text();
    if (r.at < b.size()) {
        v.color_scheme = SchemeExtension(r);
    }
    r.Done();
    CheckRequest(v);
    return v;
}

std::vector<std::uint8_t> EncodeThemeEvent(const ThemeEvent &v)
{
    CheckEvent(v);
    Writer w;
    w.U(v.request, 8);
    w.U(v.generation, 8);
    w.U(static_cast<unsigned>(v.status), 1);
    w.Text(v.id);
    w.Text(v.name);
    w.Text(v.detail);
    if (v.color_scheme != "dark") {
        SchemeExtension(w, v.color_scheme);
    }
    return w.bytes;
}

ThemeEvent DecodeThemeEvent(std::span<const std::uint8_t> b)
{
    Reader r{b};
    ThemeEvent v;
    v.request = r.U(8);
    v.generation = r.U(8);
    v.status = static_cast<ThemeStatus>(r.U(1));
    v.id = r.Text();
    v.name = r.Text();
    v.detail = r.Text();
    if (r.at < b.size()) {
        v.color_scheme = SchemeExtension(r);
    }
    r.Done();
    CheckEvent(v);
    return v;
}

std::vector<std::uint8_t> EncodeThemeApplied(const ThemeApplied &v)
{
    Check(v.generation, "Zero theme generation");
    Writer w;
    w.U(v.generation, 8);
    w.U(v.success, 1);
    w.Text(v.detail);
    return w.bytes;
}

ThemeApplied DecodeThemeApplied(std::span<const std::uint8_t> b)
{
    Reader r{b};
    ThemeApplied v;
    v.generation = r.U(8);
    v.success = r.Bool();
    v.detail = r.Text();
    r.Done();
    EncodeThemeApplied(v);
    return v;
}
} // namespace prism::contracts
