#include "dsl_tooltip_p.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "tooltip_validation_p.hpp"

#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism::runtime {
namespace {
[[noreturn]] void Error(const ComponentSource &source, int line, std::string message)
{
    throw LoadFailure({LoadStage::Semantic, source, line, std::move(message)});
}

const PreparedPropertyAssignment *Property(const PreparedNode &node, DslProperty id)
{
    for (const auto &property : node.properties) {
        if (property.id == id) {
            return &property;
        }
    }
    return nullptr;
}

bool DynamicProperty(const PreparedNode &node, DslProperty id)
{
    for (const auto &binding : node.bindings) {
        if (binding.target == id) {
            return true;
        }
    }
    for (const auto &reference : node.theme_refs) {
        if (reference.target == id) {
            return true;
        }
    }
    return false;
}

void Collect(const PreparedNode &node, std::vector<const PreparedNode *> &nodes)
{
    nodes.push_back(&node);
    for (const auto &child : node.children) {
        Collect(child, nodes);
    }
}

bool NormalAnchor(const PreparedNode &node, const PreparedNode &anchor, bool forbidden = false)
{
    forbidden = forbidden || IsFloatingKind(node.kind) || node.kind == Kind::Visual;
    if (&node == &anchor) {
        return !forbidden;
    }
    for (const auto &child : node.children) {
        if (NormalAnchor(child, anchor, forbidden)) {
            return true;
        }
    }
    return false;
}

void ValidateContents(const PreparedNode &node, std::size_t &text_bytes,
                      const ComponentSource &source, bool tooltip_root = false)
{
    if ((!tooltip_root && !IsTooltipContentKind(node.kind)) || !node.region.empty() ||
        node.gesture || node.contour || node.contour_recipe ||
        Property(node, DslProperty::Action) || DynamicProperty(node, DslProperty::Action)) {
        Error(source, node.line, "Tooltip content must remain read-only and noninteractive");
    }
    if (const auto *material = Property(node, DslProperty::Material);
        material && std::get<std::string>(material->value) == "window") {
        Error(source, node.line, "Tooltip cannot use the compositor window material");
    }
    if (const auto *text = Property(node, DslProperty::Text)) {
        const auto &value = std::get<std::string>(text->value);
        text_bytes += value.size();
        if (!ValidTooltipText(value) || text_bytes > kMaxTooltipTextBytes) {
            Error(source, node.line, "Tooltip text requires at most 256 bytes of valid UTF-8");
        }
    }
    for (const auto &child : node.children) {
        ValidateContents(child, text_bytes, source);
    }
}

void ValidateTooltip(const PreparedNode &node, const PreparedNode &root,
                     const std::vector<const PreparedNode *> &nodes, const ComponentSource &source,
                     bool unresolved_regions)
{
    bool direct_child = false;
    for (const auto &child : root.children) {
        direct_child = direct_child || &child == &node;
    }
    const auto *reference = Property(node, DslProperty::TooltipFor);
    if (!direct_child || !reference || DynamicProperty(node, DslProperty::TooltipFor) ||
        !ValidPropertyValue(DslProperty::TooltipFor, std::get<std::string>(reference->value))) {
        Error(source, node.line, "Tooltip requires a literal anchor and direct Scene root parent");
    }
    for (const auto dimension : {DslProperty::Width, DslProperty::Height}) {
        const auto *value = Property(node, dimension);
        if ((!value || std::get<double>(value->value) <= 0) && !DynamicProperty(node, dimension)) {
            Error(source, node.line, "Tooltip requires explicit positive width and height");
        }
    }

    const auto &action = std::get<std::string>(reference->value);
    std::size_t matches{};
    for (const auto *anchor : nodes) {
        const auto *value = Property(*anchor, DslProperty::Action);
        if (!value || std::get<std::string>(value->value) != action) {
            continue;
        }
        if (!NormalAnchor(root, *anchor) || DynamicProperty(*anchor, DslProperty::Action) ||
            IsFloatingKind(anchor->kind) || IsChoiceGroup(anchor->kind) ||
            anchor->kind == Kind::ScrollView ||
            !((anchor->allowed_properties & PropertyBit(DslProperty::Action)) != 0)) {
            Error(source, anchor->line, "Tooltip anchor must be a fixed action target");
        }
        ++matches;
    }
    // Only the trusted layout compiler can declare an unresolved region. Its
    // component may provide this action during critical composition. Existing
    // matches remain subject to the normal eligibility and uniqueness rules.
    if (matches != 1 && !(matches == 0 && unresolved_regions)) {
        Error(source, node.line, "Tooltip anchor action must resolve exactly once");
    }

    std::size_t text_bytes{};
    ValidateContents(node, text_bytes, source, true);
}
} // namespace

void ValidatePreparedTooltipTree(const PreparedNode &root, const ComponentSource &source,
                                 bool await_regions)
{
    std::vector<const PreparedNode *> nodes;
    Collect(root, nodes);
    bool has_tooltip = false;
    bool unresolved_regions = false;
    for (const auto *node : nodes) {
        has_tooltip = has_tooltip || node->kind == Kind::Tooltip;
        unresolved_regions =
            unresolved_regions || (await_regions && !node->region.empty() && !node->region_mounted);
    }
    if (!has_tooltip) {
        return;
    }

    bool floating = false;
    std::set<std::string> anchors;
    for (const auto &child : root.children) {
        if (IsFloatingKind(child.kind)) {
            floating = true;
        } else if (floating) {
            Error(source, child.line, "Floating declarations must follow root content");
        }
    }
    for (const auto *node : nodes) {
        if (node->kind == Kind::Tooltip) {
            ValidateTooltip(*node, root, nodes, source, unresolved_regions);
            const auto &anchor =
                std::get<std::string>(Property(*node, DslProperty::TooltipFor)->value);
            if (!anchors.insert(anchor).second) {
                Error(source, node->line, "Duplicate Tooltip anchor");
            }
        } else if (Property(*node, DslProperty::TooltipFor) ||
                   DynamicProperty(*node, DslProperty::TooltipFor) ||
                   Property(*node, DslProperty::TooltipDelayMs) ||
                   DynamicProperty(*node, DslProperty::TooltipDelayMs)) {
            Error(source, node->line, "Tooltip properties belong to Tooltip");
        }
    }
}
} // namespace prism::runtime
