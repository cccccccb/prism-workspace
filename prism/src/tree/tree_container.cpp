#include "prism/tree/tree_container.hpp"
#include "prism/ipc/wm_messages.hpp"
#include <cmath>
#include <numeric>

namespace prism::tree {

namespace {

std::vector<double> NormalizedFractions(const std::vector<std::shared_ptr<TreeNode>> &children,
                                        bool horizontal)
{
    std::vector<double> result;
    std::size_t missing = 0;
    double existing = 0;
    for (const auto &child : children) {
        const auto value = horizontal ? child->GetWidthFraction() : child->GetHeightFraction();
        result.push_back(value);
        if (value <= 0.0001) {
            ++missing;
        } else {
            existing += value;
        }
    }

    double total = 0;
    for (auto &value : result) {
        if (value <= 0.0001) {
            value = existing <= 0.0001 ? 1.0 : existing / (children.size() - missing);
        }
        total += value;
    }
    if (total > 0 && std::abs(total - 1.0) > 1e-12) {
        for (auto &value : result) {
            value /= total;
        }
    }
    return result;
}

} // namespace

void ContainerNode::NormalizeFractions()
{
    const auto &children = GetChildren();
    if (children.empty()) {
        return;
    }

    const auto widths = NormalizedFractions(children, true);
    const auto heights = NormalizedFractions(children, false);
    for (std::size_t i = 0; i < children.size(); ++i) {
        children[i]->SetFractions(widths[i], heights[i]);
    }
}

void ContainerNode::ArrangeChildren(const core::Rect &area, int inner_gap, float header_height)
{
    const auto &children = GetChildren();
    SetBounds(area);
    if (children.empty()) {
        return;
    }

    NormalizeFractions();

    switch (layout_mode_) {
    case LayoutMode::SplitHorizontal:
        ArrangeSplitHorizontal(area, inner_gap, header_height);
        break;
    case LayoutMode::SplitVertical:
        ArrangeSplitVertical(area, inner_gap, header_height);
        break;
    case LayoutMode::Tabbed:
        ArrangeTabbed(area, header_height);
        break;
    case LayoutMode::Stacked:
        ArrangeStacked(area, header_height);
        break;
    default:
        ArrangeSplitHorizontal(area, inner_gap, header_height);
        break;
    }
}

void ContainerNode::ArrangeSplitHorizontal(const core::Rect &area, int inner_gap,
                                           float header_height)
{
    const auto &children = GetChildren();
    int n = static_cast<int>(children.size());
    const float axis = std::max(0.0f, area.width);
    const float minimum = std::min(1.0f, axis / n);
    const float gap = n > 1 ? std::min(float(std::max(0, inner_gap)),
                                       std::max(0.0f, std::floor((axis - n * minimum) / (n - 1))))
                            : 0;
    const float usable_width = std::max(0.0f, axis - (n - 1) * gap);

    float cur_x = area.x;
    for (int i = 0; i < n; ++i) {
        auto &child = children[i];
        const float remaining = std::max(0.0f, area.x + axis - cur_x);
        const float desired = axis >= n
                                  ? std::round(usable_width * float(child->GetWidthFraction()))
                                  : usable_width * float(child->GetWidthFraction());
        // Reserve room for every later sibling, including their gaps. Rounding
        // one tile must never make the final tile negative or collapse it.
        const float maximum = std::max(minimum, remaining - (n - i - 1) * (minimum + gap));
        const float w = i == n - 1 ? remaining : std::clamp(desired, minimum, maximum);

        core::Rect child_rect{cur_x, area.y, w, area.height};
        child->SetBounds(child_rect);

        if (auto con = std::dynamic_pointer_cast<ContainerNode>(child)) {
            con->ArrangeChildren(child_rect, inner_gap, header_height);
        }

        cur_x += w + gap;
    }
}

void ContainerNode::ArrangeSplitVertical(const core::Rect &area, int inner_gap, float header_height)
{
    const auto &children = GetChildren();
    int n = static_cast<int>(children.size());
    const float axis = std::max(0.0f, area.height);
    const float minimum = std::min(1.0f, axis / n);
    const float gap = n > 1 ? std::min(float(std::max(0, inner_gap)),
                                       std::max(0.0f, std::floor((axis - n * minimum) / (n - 1))))
                            : 0;
    const float usable_height = std::max(0.0f, axis - (n - 1) * gap);

    float cur_y = area.y;
    for (int i = 0; i < n; ++i) {
        auto &child = children[i];
        const float remaining = std::max(0.0f, area.y + axis - cur_y);
        const float desired = axis >= n
                                  ? std::round(usable_height * float(child->GetHeightFraction()))
                                  : usable_height * float(child->GetHeightFraction());
        const float maximum = std::max(minimum, remaining - (n - i - 1) * (minimum + gap));
        const float h = i == n - 1 ? remaining : std::clamp(desired, minimum, maximum);

        core::Rect child_rect{area.x, cur_y, area.width, h};
        child->SetBounds(child_rect);

        if (auto con = std::dynamic_pointer_cast<ContainerNode>(child)) {
            con->ArrangeChildren(child_rect, inner_gap, header_height);
        }

        cur_y += h + gap;
    }
}

void ContainerNode::ArrangeTabbed(const core::Rect &area, float header_height)
{
    const auto &children = GetChildren();
    float eff_header_h = std::max(16.0f, header_height);
    core::Rect content_area{area.x, area.y + eff_header_h, area.width,
                            std::max(0.0f, area.height - eff_header_h)};

    for (size_t i = 0; i < children.size(); ++i) {
        auto &child = children[i];
        child->SetBounds(content_area);
        if (auto con = std::dynamic_pointer_cast<ContainerNode>(child)) {
            con->ArrangeChildren(content_area, 0, header_height);
        }
    }
}

void ContainerNode::ArrangeStacked(const core::Rect &area, float header_height)
{
    const auto &children = GetChildren();
    float eff_header_h = std::max(16.0f, header_height);
    float total_headers = eff_header_h * static_cast<float>(children.size());
    core::Rect content_area{area.x, area.y + total_headers, area.width,
                            std::max(0.0f, area.height - total_headers)};

    for (size_t i = 0; i < children.size(); ++i) {
        auto &child = children[i];
        child->SetBounds(content_area);
        if (auto con = std::dynamic_pointer_cast<ContainerNode>(child)) {
            con->ArrangeChildren(content_area, 0, header_height);
        }
    }
}

bool ContainerNode::AutoPrune()
{
    const auto &children = GetChildren();
    auto p = GetParent();
    if (!p) {
        return false;
    }

    if (children.empty()) {
        return p->RemoveChild(shared_from_this());
    }

    if (children.size() == 1 && p->type == NodeType::Container) {
        auto only_child = children[0];
        only_child->SetFractions(GetWidthFraction(), GetHeightFraction());
        p->ReplaceChild(shared_from_this(), only_child);
        return true;
    }

    return false;
}

ipc::TreeNodeMessage ContainerNode::ToMessage(bool is_focused) const
{
    const auto &children = GetChildren();
    ipc::TreeNodeMessage message;
    message.id = GetNodeId();
    message.type = "container";
    message.focused = is_focused;
    message.rect = ipc::RectMessage(GetBounds());
    message.fraction = ipc::FractionMessage{GetWidthFraction(), GetHeightFraction()};
    const char *layout = "splith";
    if (layout_mode_ == LayoutMode::SplitVertical) {
        layout = "splitv";
    } else if (layout_mode_ == LayoutMode::Tabbed) {
        layout = "tabbed";
    } else if (layout_mode_ == LayoutMode::Stacked) {
        layout = "stacked";
    }
    message.layout = layout;
    message.active_child_index = active_child_index_;
    message.nodes.emplace();
    for (const auto &child : children) {
        message.nodes->push_back(child->ToMessage(false));
    }
    return message;
}

std::string ContainerNode::ToJson(bool is_focused) const
{
    return nlohmann::json(ToMessage(is_focused)).dump();
}

} // namespace prism::tree
