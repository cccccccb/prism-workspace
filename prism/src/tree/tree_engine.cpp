#include "prism/tree/tree_engine.hpp"
#include "prism/wm/window.hpp"
#include <cmath>
#include <limits>
#include <sstream>
#include <nlohmann/json.hpp>

namespace prism::tree {

TreeEngine::TreeEngine() {
    active_workspace_ = GetOrCreateWorkspace("1");
    active_workspace_->SetActive(true);
}

std::shared_ptr<WorkspaceNode> TreeEngine::GetOrCreateWorkspace(const std::string& name) {
    for (const auto& ws : workspaces_) {
        if (ws->GetName() == name) return ws;
    }
    auto ws = std::make_shared<WorkspaceNode>(next_workspace_id_++, name);
    ws->GetRootContainer()->parent = ws;
    workspaces_.push_back(ws);
    return ws;
}

bool TreeEngine::SwitchWorkspace(const std::string& name) {
    auto target_ws = GetOrCreateWorkspace(name);
    if (target_ws == active_workspace_) return true;

    if (active_workspace_) {
        active_workspace_->SetActive(false);
        active_workspace_->focused_inactive_child = focused_node_;
    }

    active_workspace_ = target_ws;
    active_workspace_->SetActive(true);

    // Restore focus
    if (auto prev_focused = active_workspace_->focused_inactive_child.lock()) {
        SetFocus(prev_focused);
    } else {
        std::vector<std::shared_ptr<TreeNode>> views;
        active_workspace_->CollectViews(views);
        if (!views.empty()) {
            SetFocus(views[0]);
        } else {
            SetFocus(nullptr);
        }
    }

    return true;
}

std::shared_ptr<ViewNode> TreeEngine::InsertWindow(
    std::shared_ptr<wm::Window> win,
    Direction dir,
    std::shared_ptr<TreeNode> target
) {
    if (!win || !active_workspace_) return nullptr;

    auto view = std::make_shared<ViewNode>(win);
    auto root_con = active_workspace_->GetRootContainer();

    // 1. If active workspace is empty, insert directly into root container
    if (!root_con->HasChildren()) {
        root_con->AddChild(view);
        root_con->NormalizeFractions();
        SetFocus(view);
        return view;
    }

    // 2. Resolve target
    if (!target) target = focused_node_.lock();
    if (!target || target->GetWorkspace() != active_workspace_) {
        std::vector<std::shared_ptr<TreeNode>> views;
        active_workspace_->CollectViews(views);
        target = views.empty() ? root_con : views.back();
    }

    if (target == root_con) {
        root_con->AddChild(view);
        root_con->NormalizeFractions();
        SetFocus(view);
        return view;
    }

    auto parent_con = target->GetParentContainer();
    if (!parent_con) {
        root_con->AddChild(view);
        root_con->NormalizeFractions();
        SetFocus(view);
        return view;
    }

    LayoutMode desired_mode = (dir == Direction::Left || dir == Direction::Right)
                              ? LayoutMode::SplitHorizontal
                              : LayoutMode::SplitVertical;

    // 3. Sibling insertion if parent matches layout mode
    if (parent_con->GetLayoutMode() == desired_mode) {
        int target_idx = parent_con->GetChildIndex(target);
        int insert_idx = (dir == Direction::Right || dir == Direction::Down) ? target_idx + 1 : target_idx;
        parent_con->AddChild(view, insert_idx);
        parent_con->NormalizeFractions();
    } else {
        // 4. BSP Binary Space Partitioning Fission: wrap target into a new ContainerNode
        auto fission_con = std::make_shared<ContainerNode>(desired_mode);
        fission_con->width_fraction = target->width_fraction;
        fission_con->height_fraction = target->height_fraction;

        parent_con->ReplaceChild(target, fission_con);

        if (dir == Direction::Right || dir == Direction::Down) {
            fission_con->AddChild(target);
            fission_con->AddChild(view);
        } else {
            fission_con->AddChild(view);
            fission_con->AddChild(target);
        }

        fission_con->NormalizeFractions();
        parent_con->NormalizeFractions();
    }

    SetFocus(view);
    return view;
}

std::shared_ptr<ContainerNode> TreeEngine::GroupTabbed(
    std::shared_ptr<TreeNode> target,
    std::shared_ptr<wm::Window> new_win
) {
    if (!target || !new_win || !active_workspace_) return nullptr;

    auto view = std::make_shared<ViewNode>(new_win);
    auto parent_con = target->GetParentContainer();
    if (!parent_con) return nullptr;

    if (auto existing_con = std::dynamic_pointer_cast<ContainerNode>(target)) {
        if (existing_con->GetLayoutMode() == LayoutMode::Tabbed) {
            existing_con->AddChild(view);
            existing_con->SetActiveChildIndex(static_cast<int>(existing_con->children.size() - 1));
            SetFocus(view);
            return existing_con;
        }
    }

    auto tab_con = std::make_shared<ContainerNode>(LayoutMode::Tabbed);
    tab_con->width_fraction = target->width_fraction;
    tab_con->height_fraction = target->height_fraction;

    parent_con->ReplaceChild(target, tab_con);
    tab_con->AddChild(target);
    tab_con->AddChild(view);
    tab_con->SetActiveChildIndex(1);

    SetFocus(view);
    return tab_con;
}

bool TreeEngine::RemoveWindow(const std::shared_ptr<wm::Window>& win) {
    auto view = FindViewForWindow(win);
    if (!view) return false;

    auto parent = view->GetParent();
    if (!parent) return false;

    auto workspace = view->GetWorkspace();
    bool was_focused = (focused_node_.lock() == view);
    if (workspace && workspace->focused_inactive_child.lock() == view) workspace->focused_inactive_child.reset();
    parent->RemoveChild(view);

    // Prune redundant container nodes up the tree
    auto cur = parent;
    while (cur && cur->type == NodeType::Container) {
        auto con = std::dynamic_pointer_cast<ContainerNode>(cur);
        auto next_p = cur->GetParent();
        if (con && workspace && con != workspace->GetRootContainer()) {
            if (!con->AutoPrune()) break;
        }
        cur = next_p;
    }

    if (auto con = std::dynamic_pointer_cast<ContainerNode>(parent)) {
        con->NormalizeFractions();
    }

    // Restore focus
    if (was_focused && active_workspace_) {
        std::vector<std::shared_ptr<TreeNode>> remaining_views;
        active_workspace_->CollectViews(remaining_views);
        if (!remaining_views.empty()) {
            SetFocus(remaining_views.back());
        } else {
            SetFocus(nullptr);
        }
    }

    return true;
}

bool TreeEngine::SwapNodes(std::shared_ptr<TreeNode> a, std::shared_ptr<TreeNode> b) {
    if (!a || !b || a == b) return false;

    auto p_a = a->GetParent();
    auto p_b = b->GetParent();
    if (!p_a || !p_b) return false;

    int idx_a = p_a->GetChildIndex(a);
    int idx_b = p_b->GetChildIndex(b);
    if (idx_a < 0 || idx_b < 0) return false;

    if (p_a == p_b) {
        std::swap(p_a->children[idx_a], p_a->children[idx_b]);
        std::swap(a->width_fraction, b->width_fraction);
        std::swap(a->height_fraction, b->height_fraction);
        return true;
    }

    // Cross-container swap
    p_a->children[idx_a] = b;
    b->parent = p_a;

    p_b->children[idx_b] = a;
    a->parent = p_b;

    std::swap(a->width_fraction, b->width_fraction);
    std::swap(a->height_fraction, b->height_fraction);

    if (auto ca = std::dynamic_pointer_cast<ContainerNode>(p_a)) ca->NormalizeFractions();
    if (auto cb = std::dynamic_pointer_cast<ContainerNode>(p_b)) cb->NormalizeFractions();

    return true;
}

bool TreeEngine::SwapFocusDirection(Direction dir) {
    auto current_node = focused_node_.lock();
    if (!current_node || !active_workspace_) return false;

    std::vector<std::shared_ptr<TreeNode>> all_views;
    active_workspace_->CollectViews(all_views);
    if (all_views.size() <= 1) return false;

    float cx0 = current_node->bounds.x + current_node->bounds.width * 0.5f;
    float cy0 = current_node->bounds.y + current_node->bounds.height * 0.5f;

    std::shared_ptr<TreeNode> best_candidate;
    float min_dist = std::numeric_limits<float>::max();

    for (const auto& v : all_views) {
        if (v == current_node) continue;

        float cxi = v->bounds.x + v->bounds.width * 0.5f;
        float cyi = v->bounds.y + v->bounds.height * 0.5f;

        float dx = cxi - cx0;
        float dy = cyi - cy0;

        bool is_in_dir = false;
        float dist = 0.0f;

        switch (dir) {
            case Direction::Right:
                if (dx > 5.0f) {
                    is_in_dir = true;
                    dist = dx + 2.0f * std::abs(dy);
                }
                break;
            case Direction::Left:
                if (dx < -5.0f) {
                    is_in_dir = true;
                    dist = -dx + 2.0f * std::abs(dy);
                }
                break;
            case Direction::Down:
                if (dy > 5.0f) {
                    is_in_dir = true;
                    dist = dy + 2.0f * std::abs(dx);
                }
                break;
            case Direction::Up:
                if (dy < -5.0f) {
                    is_in_dir = true;
                    dist = -dy + 2.0f * std::abs(dx);
                }
                break;
        }

        if (is_in_dir && dist < min_dist) {
            min_dist = dist;
            best_candidate = v;
        }
    }

    if (best_candidate) {
        return SwapNodes(current_node, best_candidate);
    }
    return false;
}

bool TreeEngine::SetLayoutMode(std::shared_ptr<TreeNode> target, LayoutMode mode) {
    if (!target) target = focused_node_.lock();
    if (!target) return false;

    if (auto con = std::dynamic_pointer_cast<ContainerNode>(target)) {
        con->SetLayoutMode(mode);
        return true;
    }

    if (auto p = target->GetParentContainer()) {
        p->SetLayoutMode(mode);
        return true;
    }

    return false;
}

void TreeEngine::SetFocus(std::shared_ptr<TreeNode> node) {
    if (node && node->GetWorkspace() != active_workspace_) return;
    for (const auto& workspace : workspaces_) {
        std::vector<std::shared_ptr<TreeNode>> views;
        workspace->CollectViews(views);
        for (const auto& view : views) if (auto win = view->GetWindow()) win->SetFocused(view == node);
    }
    if (!node) {
        focused_node_.reset();
        return;
    }
    focused_node_ = node;
    if (auto ws = node->GetWorkspace()) {
        ws->focused_inactive_child = node;
    }
    auto child = node;
    while (auto p = child->GetParentContainer()) {
        p->SetActiveChildIndex(p->GetChildIndex(child));
        child = p;
    }
}

void TreeEngine::SetFocusedWindow(const std::shared_ptr<wm::Window>& win) {
    auto view = FindViewForWindow(win);
    if (view) {
        if (auto workspace = view->GetWorkspace(); workspace && workspace != active_workspace_)
            SwitchWorkspace(workspace->GetName());
        SetFocus(view);
    }
}

bool TreeEngine::SplitFocused(LayoutMode mode) {
    if (mode != LayoutMode::SplitHorizontal && mode != LayoutMode::SplitVertical) return false;
    auto target = focused_node_.lock();
    if (!target) {
        if (!active_workspace_) return false;
        active_workspace_->GetRootContainer()->SetLayoutMode(mode);
        return true;
    }
    auto parent = target->GetParentContainer();
    if (!parent) return false;
    if (parent->children.size() == 1) { parent->SetLayoutMode(mode); return true; }
    auto container = std::make_shared<ContainerNode>(mode);
    container->width_fraction = target->width_fraction;
    container->height_fraction = target->height_fraction;
    parent->ReplaceChild(target, container);
    container->AddChild(target);
    container->NormalizeFractions();
    SetFocus(target);
    return true;
}

bool TreeEngine::MoveWindowToWorkspace(const std::shared_ptr<wm::Window>& win, const std::string& name) {
    auto view = FindViewForWindow(win);
    if (!view || name.empty()) return false;
    auto destination = GetOrCreateWorkspace(name);
    if (view->GetWorkspace() == destination) return true;
    if (!RemoveWindow(win)) return false;
    // Keep the same view node identity when only its workspace changes.
    view->width_fraction = 0;
    view->height_fraction = 0;
    destination->GetRootContainer()->AddChild(view);
    destination->GetRootContainer()->NormalizeFractions();
    destination->focused_inactive_child = view;
    return true;
}

std::shared_ptr<wm::Window> TreeEngine::GetFocusedWindow() const {
    auto cur = focused_node_.lock();
    return cur ? cur->GetWindow() : nullptr;
}

bool TreeEngine::MoveFocus(Direction dir) {
    auto current_node = focused_node_.lock();
    if (!current_node || !active_workspace_) return false;

    std::vector<std::shared_ptr<TreeNode>> all_views;
    active_workspace_->CollectViews(all_views);
    if (all_views.size() <= 1) return false;

    float cx0 = current_node->bounds.x + current_node->bounds.width * 0.5f;
    float cy0 = current_node->bounds.y + current_node->bounds.height * 0.5f;

    std::shared_ptr<TreeNode> best_candidate;
    float min_dist = std::numeric_limits<float>::max();

    for (const auto& v : all_views) {
        if (v == current_node) continue;

        float cxi = v->bounds.x + v->bounds.width * 0.5f;
        float cyi = v->bounds.y + v->bounds.height * 0.5f;

        float dx = cxi - cx0;
        float dy = cyi - cy0;

        bool is_in_dir = false;
        float dist = 0.0f;

        switch (dir) {
            case Direction::Right:
                if (dx > 5.0f) {
                    is_in_dir = true;
                    dist = dx + 2.0f * std::abs(dy);
                }
                break;
            case Direction::Left:
                if (dx < -5.0f) {
                    is_in_dir = true;
                    dist = -dx + 2.0f * std::abs(dy);
                }
                break;
            case Direction::Down:
                if (dy > 5.0f) {
                    is_in_dir = true;
                    dist = dy + 2.0f * std::abs(dx);
                }
                break;
            case Direction::Up:
                if (dy < -5.0f) {
                    is_in_dir = true;
                    dist = -dy + 2.0f * std::abs(dx);
                }
                break;
        }

        if (is_in_dir && dist < min_dist) {
            min_dist = dist;
            best_candidate = v;
        }
    }

    if (best_candidate) {
        SetFocus(best_candidate);
        return true;
    }

    return false;
}

void TreeEngine::Arrange(const core::Rect& screen_area, const decoration::TilingDecorationSpec& spec) {
    if (!active_workspace_) return;
    active_workspace_->Arrange(
        screen_area,
        spec.gaps.inner,
        spec.gaps.outer,
        spec.gaps.smart_gaps,
        spec.header.height
    );
}

std::vector<std::pair<std::shared_ptr<wm::Window>, core::Rect>> TreeEngine::GetCalculatedLayout() const {
    std::vector<std::pair<std::shared_ptr<wm::Window>, core::Rect>> result;
    if (!active_workspace_) return result;

    std::vector<std::shared_ptr<TreeNode>> views;
    active_workspace_->CollectViews(views);

    for (const auto& v : views) {
        if (auto win = v->GetWindow()) {
            result.emplace_back(win, v->bounds);
        }
    }
    return result;
}

std::shared_ptr<ViewNode> TreeEngine::FindViewForWindow(const std::shared_ptr<wm::Window>& win) const {
    if (!win) return nullptr;
    for (const auto& ws : workspaces_) {
        std::vector<std::shared_ptr<TreeNode>> views;
        ws->CollectViews(views);
        for (const auto& v : views) {
            if (v->GetWindow() == win) {
                return std::dynamic_pointer_cast<ViewNode>(v);
            }
        }
    }
    return nullptr;
}

std::string TreeEngine::DumpTreeJson() const {
    std::ostringstream ss;
    auto focused = focused_node_.lock();
    ss << "{\"type\":\"root\",\"active_workspace\":"
       << nlohmann::json(active_workspace_ ? active_workspace_->GetName() : "").dump()
       << ",\"focused_id\":" << (focused ? reinterpret_cast<uintptr_t>(focused.get()) : 0)
       << ",\"workspaces\":[";
    for (size_t i = 0; i < workspaces_.size(); ++i) {
        if (i > 0) ss << ",";
        ss << workspaces_[i]->ToJson(workspaces_[i] == active_workspace_);
    }
    ss << "]}";
    return ss.str();
}

} // namespace prism::tree
