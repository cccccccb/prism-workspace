#include "scene_p.hpp"
#include "tooltip_validation_p.hpp"

#include <set>
#include <stdexcept>
#include <string>

namespace prism::runtime {
namespace {
template <typename Node> bool DynamicProperty(const Node &node, DslProperty id)
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

template <typename Node>
void ValidateContents(const Node &node, std::size_t &text_bytes, bool tooltip_root = false)
{
    if ((!tooltip_root && !IsTooltipContentKind(node.kind)) || !node.region.empty() ||
        node.gesture || node.contour_source || node.contour_recipe || !node.action.empty() ||
        node.properties.contains(DslProperty::Action) ||
        DynamicProperty(node, DslProperty::Action) || node.style.material == "window") {
        throw std::invalid_argument("Tooltip content must remain read-only and noninteractive");
    }
    text_bytes += node.text.size();
    if (!ValidTooltipText(node.text) || text_bytes > kMaxTooltipTextBytes) {
        throw std::invalid_argument("Tooltip text requires at most 256 bytes of valid UTF-8");
    }
    for (const auto &child : node.children) {
        ValidateContents(*child, text_bytes);
    }
}
} // namespace

void Scene::ValidateTooltipTree() const
{
    std::set<std::string> anchors;
    bool floating = false;
    for (const auto &child : root_->children) {
        if (IsFloatingKind(child->kind)) {
            floating = true;
        } else if (floating) {
            throw std::invalid_argument("Floating declarations must follow root content");
        }
    }
    for (const auto *node : nodes_) {
        if (!node) {
            continue;
        }
        if (node->kind != Kind::Tooltip) {
            if (!node->tooltip_for.empty() || node->properties.contains(DslProperty::TooltipFor) ||
                node->properties.contains(DslProperty::TooltipDelayMs) ||
                DynamicProperty(*node, DslProperty::TooltipFor) ||
                DynamicProperty(*node, DslProperty::TooltipDelayMs)) {
                throw std::invalid_argument("Tooltip properties belong to Tooltip");
            }
            continue;
        }
        if (node->parent != root_.get() ||
            !ValidPropertyValue(DslProperty::TooltipFor, node->tooltip_for) ||
            !anchors.insert(node->tooltip_for).second || node->style.width <= 0 ||
            node->style.height <= 0 || DynamicProperty(*node, DslProperty::TooltipFor)) {
            throw std::invalid_argument(
                "Tooltip requires a unique literal anchor and explicit size");
        }

        std::size_t matches{};
        for (const auto *anchor : nodes_) {
            if (!anchor || anchor->action != node->tooltip_for) {
                continue;
            }
            if (DynamicProperty(*anchor, DslProperty::Action) || IsFloatingKind(anchor->kind) ||
                IsChoiceGroup(anchor->kind) || anchor->kind == Kind::ScrollView) {
                throw std::invalid_argument("Tooltip anchor must be a fixed action target");
            }
            for (auto *parent = anchor; parent; parent = parent->parent) {
                if (IsFloatingKind(parent->kind) || parent->kind == Kind::Visual) {
                    throw std::invalid_argument(
                        "Tooltip anchor must belong to normal Scene content");
                }
            }
            ++matches;
        }
        if (matches != 1) {
            throw std::invalid_argument("Tooltip anchor action must resolve exactly once");
        }

        std::size_t text_bytes{};
        ValidateContents(*node, text_bytes, true);
    }
}
} // namespace prism::runtime
