#include "prism/tree/tree_container.hpp"
#include <cmath>
#include <numeric>
#include <sstream>

namespace prism::tree {

void ContainerNode::NormalizeFractions() {
    if (children.empty()) return;

    // 1. Width fraction balancing (Sway algorithm)
    int new_w = 0;
    double cur_w = 0.0;
    for (const auto& c : children) {
        if (c->width_fraction <= 0.0001) new_w++;
        else cur_w += c->width_fraction;
    }

    double total_w = 0.0;
    int n = static_cast<int>(children.size());
    for (auto& c : children) {
        if (c->width_fraction <= 0.0001) {
            if (cur_w <= 0.0001) c->width_fraction = 1.0;
            else if (n > new_w) c->width_fraction = cur_w / (n - new_w);
            else c->width_fraction = cur_w;
        }
        total_w += c->width_fraction;
    }
    if (total_w > 0.0) {
        for (auto& c : children) c->width_fraction /= total_w;
    }

    // 2. Height fraction balancing (Sway algorithm)
    int new_h = 0;
    double cur_h = 0.0;
    for (const auto& c : children) {
        if (c->height_fraction <= 0.0001) new_h++;
        else cur_h += c->height_fraction;
    }

    double total_h = 0.0;
    for (auto& c : children) {
        if (c->height_fraction <= 0.0001) {
            if (cur_h <= 0.0001) c->height_fraction = 1.0;
            else if (n > new_h) c->height_fraction = cur_h / (n - new_h);
            else c->height_fraction = cur_h;
        }
        total_h += c->height_fraction;
    }
    if (total_h > 0.0) {
        for (auto& c : children) c->height_fraction /= total_h;
    }
}

void ContainerNode::ArrangeChildren(const core::Rect& area, int inner_gap, float header_height) {
    bounds = area;
    if (children.empty()) return;

    NormalizeFractions();

    switch (layout_mode_) {
        case LayoutMode::SplitHorizontal:
            ArrangeSplitHorizontal(area, inner_gap);
            break;
        case LayoutMode::SplitVertical:
            ArrangeSplitVertical(area, inner_gap);
            break;
        case LayoutMode::Tabbed:
            ArrangeTabbed(area, header_height);
            break;
        case LayoutMode::Stacked:
            ArrangeStacked(area, header_height);
            break;
        default:
            ArrangeSplitHorizontal(area, inner_gap);
            break;
    }
}

void ContainerNode::ArrangeSplitHorizontal(const core::Rect& area, int inner_gap) {
    int n = static_cast<int>(children.size());
    float total_gaps = static_cast<float>(std::max(0, n - 1) * inner_gap);
    float usable_width = std::max(0.0f, area.width - total_gaps);

    float cur_x = area.x;
    for (int i = 0; i < n; ++i) {
        auto& child = children[i];
        float w = (i == n - 1) ? (area.x + area.width - cur_x)
                               : std::round(usable_width * static_cast<float>(child->width_fraction));

        core::Rect child_rect{cur_x, area.y, w, area.height};
        child->bounds = child_rect;

        if (auto con = std::dynamic_pointer_cast<ContainerNode>(child)) {
            con->ArrangeChildren(child_rect, inner_gap, 32.0f);
        }

        cur_x += w + static_cast<float>(inner_gap);
    }
}

void ContainerNode::ArrangeSplitVertical(const core::Rect& area, int inner_gap) {
    int n = static_cast<int>(children.size());
    float total_gaps = static_cast<float>(std::max(0, n - 1) * inner_gap);
    float usable_height = std::max(0.0f, area.height - total_gaps);

    float cur_y = area.y;
    for (int i = 0; i < n; ++i) {
        auto& child = children[i];
        float h = (i == n - 1) ? (area.y + area.height - cur_y)
                               : std::round(usable_height * static_cast<float>(child->height_fraction));

        core::Rect child_rect{area.x, cur_y, area.width, h};
        child->bounds = child_rect;

        if (auto con = std::dynamic_pointer_cast<ContainerNode>(child)) {
            con->ArrangeChildren(child_rect, inner_gap, 32.0f);
        }

        cur_y += h + static_cast<float>(inner_gap);
    }
}

void ContainerNode::ArrangeTabbed(const core::Rect& area, float header_height) {
    float eff_header_h = std::max(16.0f, header_height);
    core::Rect content_area{
        area.x,
        area.y + eff_header_h,
        area.width,
        std::max(0.0f, area.height - eff_header_h)
    };

    for (size_t i = 0; i < children.size(); ++i) {
        auto& child = children[i];
        child->bounds = content_area;
        if (auto con = std::dynamic_pointer_cast<ContainerNode>(child)) {
            con->ArrangeChildren(content_area, 0, header_height);
        }
    }
}

void ContainerNode::ArrangeStacked(const core::Rect& area, float header_height) {
    float eff_header_h = std::max(16.0f, header_height);
    float total_headers = eff_header_h * static_cast<float>(children.size());
    core::Rect content_area{
        area.x,
        area.y + total_headers,
        area.width,
        std::max(0.0f, area.height - total_headers)
    };

    for (size_t i = 0; i < children.size(); ++i) {
        auto& child = children[i];
        child->bounds = content_area;
        if (auto con = std::dynamic_pointer_cast<ContainerNode>(child)) {
            con->ArrangeChildren(content_area, 0, header_height);
        }
    }
}

bool ContainerNode::AutoPrune() {
    auto p = parent.lock();
    if (!p) return false;

    if (children.empty()) {
        return p->RemoveChild(shared_from_this());
    }

    if (children.size() == 1 && p->type == NodeType::Container) {
        auto only_child = children[0];
        p->ReplaceChild(shared_from_this(), only_child);
        return true;
    }

    return false;
}

std::string ContainerNode::ToJson(bool is_focused) const {
    std::ostringstream ss;
    const char* layout_str = "splith";
    if (layout_mode_ == LayoutMode::SplitVertical) layout_str = "splitv";
    else if (layout_mode_ == LayoutMode::Tabbed) layout_str = "tabbed";
    else if (layout_mode_ == LayoutMode::Stacked) layout_str = "stacked";

    ss << "{\"id\":" << reinterpret_cast<uintptr_t>(this)
       << ",\"type\":\"container\""
       << ",\"layout\":\"" << layout_str << "\""
       << ",\"focused\":" << (is_focused ? "true" : "false")
       << ",\"rect\":{\"x\":" << bounds.x << ",\"y\":" << bounds.y
       << ",\"width\":" << bounds.width << ",\"height\":" << bounds.height << "}"
       << ",\"active_child_index\":" << active_child_index_
       << ",\"fraction\":{\"width\":" << width_fraction << ",\"height\":" << height_fraction << "}"
       << ",\"nodes\":[";
    for (size_t i = 0; i < children.size(); ++i) {
        if (i > 0) ss << ",";
        ss << children[i]->ToJson(false);
    }
    ss << "]}";
    return ss.str();
}

} // namespace prism::tree
