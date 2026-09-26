#pragma once
#include "prism/contracts/theme.hpp"

namespace prism::test {
// Deliberately independent of a production theme or DSL compiler: native WM
// tests assert the typed contract and configure geometry they actually install.
inline contracts::ThemeSnapshot WmThemeFixture(std::uint64_t generation=1) {
    contracts::ThemeSnapshot t;
    t.generation=generation;t.id="wm-fixture";t.name="WM Fixture";
    t.layout={52,100,620,14,12};
    t.normal={true,12,1,{255,255,255,70},18,4,{0,0,0,80}};
    t.focused={true,12,1.5,{90,150,240,180},18,4,{0,0,0,90}};
    t.fullscreen.enabled=false;
    return t;
}
}
