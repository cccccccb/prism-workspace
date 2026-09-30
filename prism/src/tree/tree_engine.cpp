#include "prism/tree/tree_engine.hpp"
#include "prism/contracts/layout_snapshot.hpp"
#include "prism/ipc/wm_messages.hpp"
#include "prism/wm/window.hpp"
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>

namespace prism::tree {

// Each boundary consumes an adjacent pair under a container. A forest has fewer
// boundaries than non-workspace nodes, including after a tabbed/split mode change.
static_assert(contracts::kMaxLayoutBoundaries >= contracts::kMaxLayoutNodes);

bool TreeEngine::HasNodeCapacity(std::size_t additional) const
{
    return revisions_->nodes <= contracts::kMaxLayoutNodes &&
           additional <= contracts::kMaxLayoutNodes - revisions_->nodes;
}

TreeEngine::TreeEngine()
{
    active_workspace_ = GetOrCreateWorkspace("1");
    active_workspace_->SetActive(true);
}

std::shared_ptr<WorkspaceNode> TreeEngine::GetOrCreateWorkspace(const std::string &name)
{
    for (const auto &ws : workspaces_) {
        if (ws->GetName() == name) {
            return ws;
        }
    }
    if (workspaces_.size() >= contracts::kMaxLayoutWorkspaces || !HasNodeCapacity(1)) {
        return nullptr;
    }

    auto ws = std::make_shared<WorkspaceNode>(next_workspace_id_++, name);
    ws->AttachRevisionState(revisions_);
    ++revisions_->topology;
    ++revisions_->layout;
    workspaces_.push_back(ws);
    return ws;
}

bool TreeEngine::SwitchWorkspace(const std::string &name)
{
    auto target_ws = GetOrCreateWorkspace(name);
    if (!target_ws) {
        return false;
    }
    if (target_ws == active_workspace_) {
        return true;
    }

    if (active_workspace_) {
        active_workspace_->SetActive(false);
        active_workspace_->focused_inactive_child = focused_node_;
    }

    active_workspace_ = target_ws;
    active_workspace_->SetActive(true);

    // Restore focus
    if (auto prev_focused = active_workspace_->focused_inactive_child.lock();
        prev_focused && prev_focused->GetWorkspace() == active_workspace_) {
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

std::shared_ptr<ViewNode> TreeEngine::InsertWindow(std::shared_ptr<wm::Window> win, Direction dir,
                                                   std::shared_ptr<TreeNode> target)
{
    if (!win || !active_workspace_ || !HasNodeCapacity(1)) {
        return nullptr;
    }

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
    if (!target) {
        target = focused_node_.lock();
    }
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
        int insert_idx =
            (dir == Direction::Right || dir == Direction::Down) ? target_idx + 1 : target_idx;
        parent_con->AddChild(view, insert_idx);
        parent_con->NormalizeFractions();
    } else {
        if (!HasNodeCapacity(2)) {
            return nullptr;
        }

        // 4. BSP Binary Space Partitioning Fission: wrap target into a new ContainerNode
        auto fission_con = std::make_shared<ContainerNode>(desired_mode);
        fission_con->SetFractions(target->GetWidthFraction(), target->GetHeightFraction());

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

std::shared_ptr<ContainerNode> TreeEngine::GroupTabbed(std::shared_ptr<TreeNode> target,
                                                       std::shared_ptr<wm::Window> new_win)
{
    if (!target || !new_win || !active_workspace_ || target->GetWorkspace() != active_workspace_) {
        return nullptr;
    }

    auto parent_con = target->GetParentContainer();
    if (!parent_con) {
        return nullptr;
    }

    const auto existing_con = std::dynamic_pointer_cast<ContainerNode>(target);
    const bool append = existing_con && existing_con->GetLayoutMode() == LayoutMode::Tabbed;
    if (!HasNodeCapacity(append ? 1 : 2)) {
        return nullptr;
    }

    auto view = std::make_shared<ViewNode>(new_win);
    if (existing_con) {
        if (append) {
            existing_con->AddChild(view);
            existing_con->SetActiveChildIndex(
                static_cast<int>(existing_con->GetChildren().size() - 1));
            SetFocus(view);
            return existing_con;
        }
    }

    auto tab_con = std::make_shared<ContainerNode>(LayoutMode::Tabbed);
    tab_con->SetFractions(target->GetWidthFraction(), target->GetHeightFraction());

    parent_con->ReplaceChild(target, tab_con);
    tab_con->AddChild(target);
    tab_con->AddChild(view);
    tab_con->SetActiveChildIndex(1);

    SetFocus(view);
    return tab_con;
}

bool TreeEngine::RemoveWindow(const std::shared_ptr<wm::Window> &win)
{
    auto view = FindViewForWindow(win);
    if (!view) {
        return false;
    }

    auto parent = view->GetParent();
    if (!parent) {
        return false;
    }

    auto workspace = view->GetWorkspace();
    bool was_focused = (focused_node_.lock() == view);
    if (workspace && workspace->focused_inactive_child.lock() == view) {
        workspace->focused_inactive_child.reset();
    }
    parent->RemoveChild(view);

    // Prune redundant container nodes up the tree
    auto cur = parent;
    while (cur && cur->type == NodeType::Container) {
        auto con = std::dynamic_pointer_cast<ContainerNode>(cur);
        auto next_p = cur->GetParent();
        if (con && workspace && con != workspace->GetRootContainer()) {
            if (!con->AutoPrune()) {
                break;
            }
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

bool TreeEngine::SwapNodes(std::shared_ptr<TreeNode> a, std::shared_ptr<TreeNode> b)
{
    if (!a || !b || a == b || a->revisions_ != revisions_ || b->revisions_ != revisions_) {
        return false;
    }

    auto p_a = a->GetParent();
    auto p_b = b->GetParent();
    if (!p_a || !p_b || !p_a->IsContainer() || !p_b->IsContainer()) {
        return false;
    }

    if (!a->SwapWith(b)) {
        return false;
    }

    if (auto ca = std::dynamic_pointer_cast<ContainerNode>(p_a)) {
        ca->NormalizeFractions();
    }
    if (auto cb = std::dynamic_pointer_cast<ContainerNode>(p_b)) {
        cb->NormalizeFractions();
    }

    const auto focused = focused_node_.lock();
    if (focused && focused->GetWorkspace() == active_workspace_) {
        SetFocus(focused);
    } else if (focused && active_workspace_) {
        std::vector<std::shared_ptr<TreeNode>> views;
        active_workspace_->CollectViews(views);
        SetFocus(views.empty() ? nullptr : views.front());
    }

    return true;
}

bool TreeEngine::SwapFocusDirection(Direction dir)
{
    auto current_node = focused_node_.lock();
    if (!current_node || !active_workspace_) {
        return false;
    }

    std::vector<std::shared_ptr<TreeNode>> all_views;
    active_workspace_->CollectViews(all_views);
    if (all_views.size() <= 1) {
        return false;
    }

    float cx0 = current_node->GetBounds().x + current_node->GetBounds().width * 0.5f;
    float cy0 = current_node->GetBounds().y + current_node->GetBounds().height * 0.5f;

    std::shared_ptr<TreeNode> best_candidate;
    float min_dist = std::numeric_limits<float>::max();

    for (const auto &v : all_views) {
        if (v == current_node) {
            continue;
        }

        float cxi = v->GetBounds().x + v->GetBounds().width * 0.5f;
        float cyi = v->GetBounds().y + v->GetBounds().height * 0.5f;

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

bool TreeEngine::SetLayoutMode(std::shared_ptr<TreeNode> target, LayoutMode mode)
{
    if (mode != LayoutMode::SplitHorizontal && mode != LayoutMode::SplitVertical &&
        mode != LayoutMode::Tabbed && mode != LayoutMode::Stacked) {
        return false;
    }
    if (!target) {
        target = focused_node_.lock();
    }
    if (!target || target->revisions_ != revisions_) {
        return false;
    }

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

void TreeEngine::SetFocus(std::shared_ptr<TreeNode> node)
{
    if (node && node->GetWorkspace() != active_workspace_) {
        return;
    }
    if (focused_node_.lock() != node) {
        ++revisions_->focus;
    }

    for (const auto &workspace : workspaces_) {
        std::vector<std::shared_ptr<TreeNode>> views;
        workspace->CollectViews(views);
        for (const auto &view : views) {
            if (auto win = view->GetWindow()) {
                win->SetFocused(view == node);
            }
        }
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

void TreeEngine::SetFocusedWindow(const std::shared_ptr<wm::Window> &win)
{
    auto view = FindViewForWindow(win);
    if (view) {
        if (auto workspace = view->GetWorkspace(); workspace && workspace != active_workspace_) {
            SwitchWorkspace(workspace->GetName());
        }
        SetFocus(view);
    }
}

bool TreeEngine::SplitFocused(LayoutMode mode)
{
    if (mode != LayoutMode::SplitHorizontal && mode != LayoutMode::SplitVertical) {
        return false;
    }
    auto target = focused_node_.lock();
    if (!target) {
        if (!active_workspace_) {
            return false;
        }
        active_workspace_->GetRootContainer()->SetLayoutMode(mode);
        return true;
    }
    auto parent = target->GetParentContainer();
    if (!parent) {
        return false;
    }
    if (parent->GetChildren().size() == 1) {
        parent->SetLayoutMode(mode);
        return true;
    }
    if (!HasNodeCapacity(1)) {
        return false;
    }

    auto container = std::make_shared<ContainerNode>(mode);
    container->SetFractions(target->GetWidthFraction(), target->GetHeightFraction());
    parent->ReplaceChild(target, container);
    container->AddChild(target);
    container->NormalizeFractions();
    SetFocus(target);
    return true;
}

bool TreeEngine::MoveWindowToWorkspace(const std::shared_ptr<wm::Window> &win,
                                       const std::string &name)
{
    auto view = FindViewForWindow(win);
    if (!view || name.empty()) {
        return false;
    }
    auto destination = GetOrCreateWorkspace(name);
    if (!destination) {
        return false;
    }
    if (view->GetWorkspace() == destination) {
        return true;
    }
    if (!RemoveWindow(win)) {
        return false;
    }
    // Keep the same view node identity when only its workspace changes.
    view->SetFractions(0, 0);
    destination->GetRootContainer()->AddChild(view);
    destination->GetRootContainer()->NormalizeFractions();
    destination->focused_inactive_child = view;
    return true;
}

std::shared_ptr<wm::Window> TreeEngine::GetFocusedWindow() const
{
    auto cur = focused_node_.lock();
    return cur ? cur->GetWindow() : nullptr;
}

bool TreeEngine::MoveFocus(Direction dir)
{
    auto current_node = focused_node_.lock();
    if (!current_node || !active_workspace_) {
        return false;
    }

    std::vector<std::shared_ptr<TreeNode>> all_views;
    active_workspace_->CollectViews(all_views);
    if (all_views.size() <= 1) {
        return false;
    }

    float cx0 = current_node->GetBounds().x + current_node->GetBounds().width * 0.5f;
    float cy0 = current_node->GetBounds().y + current_node->GetBounds().height * 0.5f;

    std::shared_ptr<TreeNode> best_candidate;
    float min_dist = std::numeric_limits<float>::max();

    for (const auto &v : all_views) {
        if (v == current_node) {
            continue;
        }

        float cxi = v->GetBounds().x + v->GetBounds().width * 0.5f;
        float cyi = v->GetBounds().y + v->GetBounds().height * 0.5f;

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

void TreeEngine::Arrange(const core::Rect &screen_area, const TreeLayoutConfig &config)
{
    if (!active_workspace_) {
        return;
    }
    active_workspace_->Arrange(screen_area, config.inner_gap, config.outer_gap, config.smart_gaps,
                               config.header_height);
}

std::vector<std::pair<std::shared_ptr<wm::Window>, core::Rect>>
TreeEngine::GetCalculatedLayout() const
{
    std::vector<std::pair<std::shared_ptr<wm::Window>, core::Rect>> result;
    if (!active_workspace_) {
        return result;
    }

    std::vector<std::shared_ptr<TreeNode>> views;
    active_workspace_->CollectViews(views);

    for (const auto &v : views) {
        if (auto win = v->GetWindow()) {
            result.emplace_back(win, v->GetBounds());
        }
    }
    return result;
}

std::shared_ptr<ViewNode>
TreeEngine::FindViewForWindow(const std::shared_ptr<wm::Window> &win) const
{
    if (!win) {
        return nullptr;
    }
    for (const auto &ws : workspaces_) {
        std::vector<std::shared_ptr<TreeNode>> views;
        ws->CollectViews(views);
        for (const auto &v : views) {
            if (v->GetWindow() == win) {
                return std::dynamic_pointer_cast<ViewNode>(v);
            }
        }
    }
    return nullptr;
}

std::string TreeEngine::DumpTreeJson() const
{
    ipc::TreeMessage message;
    auto focused = focused_node_.lock();
    message.active_workspace = active_workspace_ ? active_workspace_->GetName() : "";
    message.focused_id = focused ? focused->GetNodeId() : 0;
    for (const auto &workspace : workspaces_) {
        message.workspaces.push_back(workspace->ToMessage(workspace == active_workspace_));
    }
    return nlohmann::json(message).dump();
}

} // namespace prism::tree
