#pragma once

#include "prism/runtime/property.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace prism::runtime {

enum class Kind { Row, Column, Box, Text, Image, Icon, IconButton, Progress, Toggle, Separator };

struct Blueprint {
    Kind kind{Kind::Box};
    std::string region;
    bool region_mounted{false};
    std::vector<PropertyAssignment> properties;
    std::vector<PropertyBinding> bindings;
    std::vector<ThemeRef> theme_refs;
    std::uint64_t allowed_properties{UINT64_MAX};
    std::vector<Blueprint> children;
};

} // namespace prism::runtime
