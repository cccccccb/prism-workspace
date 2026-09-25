#include "prism/runtime/scene.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace prism::runtime {

struct Scene::Node {
    contracts::NodeId id{};
    Kind kind{Kind::Box};
    Style style{};
    std::string text;
    std::string slot;
    std::string action;
    contracts::LogicalRect bounds{};
    ShapedText shaped{};
    std::vector<std::unique_ptr<Node>> children;
};

namespace {
bool Inside(contracts::LogicalRect r, contracts::LogicalPoint p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height;
}
bool ValidSize(contracts::LogicalSize s) {
    return std::isfinite(s.width) && std::isfinite(s.height) && s.width > 0 && s.height > 0 &&
           s.width <= 16384 && s.height <= 16384;
}
bool SameColor(contracts::Color a, contracts::Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
} // namespace

Scene::~Scene() = default;

Scene::Scene(Blueprint root, ShapeText shaper, contracts::ResourceId font)
    : shaper_(std::move(shaper)), font_(font) {
    if (!shaper_) throw std::invalid_argument("Scene requires a text shaper");
    root_ = MakeNode(std::move(root));
}

std::unique_ptr<Scene::Node> Scene::MakeNode(Blueprint blueprint) {
    auto node = std::make_unique<Node>();
    if (nodes_.size() >= UINT32_MAX) throw std::length_error("Scene node limit");
    node->id = {static_cast<std::uint32_t>(nodes_.size()), 1};
    Node* raw = node.get();
    nodes_.push_back(raw);
    node->kind = blueprint.kind;
    node->style = blueprint.style;
    node->text = std::move(blueprint.text);
    node->slot = std::move(blueprint.slot);
    node->action = std::move(blueprint.action);
    for (auto& child : blueprint.children) node->children.push_back(MakeNode(std::move(child)));
    return node;
}

Scene::Node* Scene::Find(contracts::NodeId id) const {
    if (!id || id.index >= nodes_.size()) return nullptr;
    Node* node = nodes_[id.index];
    return node->id == id ? node : nullptr;
}

contracts::NodeId Scene::RootId() const { return root_->id; }
contracts::LogicalRect Scene::Bounds(contracts::NodeId id) const {
    Node* node = Find(id);
    return node ? node->bounds : contracts::LogicalRect{};
}

bool Scene::SetSlot(std::string_view name, std::string value) {
    bool changed = false;
    for (Node* node : nodes_) {
        if (node->slot == name && node->text != value) {
            node->text = value;
            dirty_ = dirty_ | Dirty::Layout | Dirty::Paint;
            changed = true;
        }
    }
    return changed;
}

bool Scene::SetViewport(contracts::LogicalSize size) {
    if (!ValidSize(size)) return false;
    if (viewport_.width != size.width || viewport_.height != size.height) {
        viewport_ = size;
        dirty_ = dirty_ | Dirty::Layout | Dirty::Paint;
    }
    return true;
}

bool Scene::SetBackground(contracts::NodeId id, contracts::Color color) {
    Node* node = Find(id);
    if (!node) return false;
    if (!SameColor(node->style.background, color)) {
        node->style.background = color;
        dirty_ = dirty_ | Dirty::Paint;
    }
    return true;
}

void Scene::Layout(Node& node, contracts::LogicalRect bounds) {
    node.bounds = bounds;
    if (node.kind == Kind::Text) {
        node.shaped = shaper_(node.text, node.style.font_size);
        return;
    }
    const double pad = std::max(0.0, node.style.padding);
    const double x = bounds.x + pad, y = bounds.y + pad;
    const double width = std::max(0.0, bounds.width - 2 * pad);
    const double height = std::max(0.0, bounds.height - 2 * pad);
    if (node.kind == Kind::Box) {
        for (auto& child : node.children) {
            auto& s = child->style;
            const double w = s.width > 0 ? std::min(s.width, width) : width;
            const double h = s.height > 0 ? std::min(s.height, height)
                : child->kind == Kind::Text ? std::min(s.font_size * 1.4, height) : height;
            Layout(*child, {x, y, w, h});
        }
        return;
    }
    const bool row = node.kind == Kind::Row;
    const double main = row ? width : height;
    const double cross = row ? height : width;
    const double gap = std::max(0.0, node.style.spacing);
    const double total_gap = gap * (node.children.empty() ? 0 : node.children.size() - 1);
    double fixed = 0;
    std::size_t flexible = 0;
    for (const auto& child : node.children) {
        const double explicit_size = row ? child->style.width : child->style.height;
        if (explicit_size > 0) fixed += explicit_size;
        else if (!row && child->kind == Kind::Text) fixed += child->style.font_size * 1.4;
        else ++flexible;
    }
    const double remaining = std::max(0.0, main - total_gap - fixed);
    const double flex_size = flexible ? remaining / flexible : 0;
    double cursor = row ? x : y;
    for (auto& child : node.children) {
        const double explicit_size = row ? child->style.width : child->style.height;
        const double intrinsic = !row && child->kind == Kind::Text ? child->style.font_size * 1.4 : flex_size;
        const double length = std::max(0.0, std::min(explicit_size > 0 ? explicit_size : intrinsic,
            std::max(0.0, (row ? x + width : y + height) - cursor)));
        const double cross_explicit = row ? child->style.height : child->style.width;
        const double other = std::min(cross_explicit > 0 ? cross_explicit : cross, cross);
        Layout(*child, row ? contracts::LogicalRect{cursor, y, length, other}
                           : contracts::LogicalRect{x, cursor, other, length});
        cursor += length + gap;
    }
}

void Scene::Paint(const Node& node, contracts::DisplayList& list) const {
    if (node.bounds.width <= 0 || node.bounds.height <= 0) return;
    if (node.style.clip) list.commands.emplace_back(contracts::PushClipRect{node.bounds});
    if (node.style.background.a) {
        if (node.style.radius > 0)
            list.commands.emplace_back(contracts::FillRoundedRect{node.bounds, node.style.radius, node.style.background});
        else list.commands.emplace_back(contracts::FillRect{node.bounds, node.style.background});
    }
    if (node.kind == Kind::Text && !node.shaped.glyphs.empty()) {
        contracts::DrawGlyphRun run;
        run.font = font_;
        run.color = node.style.foreground;
        for (auto glyph : node.shaped.glyphs) {
            glyph.origin.x += node.bounds.x;
            glyph.origin.y += node.bounds.y;
            run.glyphs.push_back(glyph);
        }
        list.commands.emplace_back(std::move(run));
    }
    for (const auto& child : node.children) Paint(*child, list);
    if (node.style.clip) list.commands.emplace_back(contracts::PopClip{});
}

std::optional<contracts::DisplayList> Scene::Build(contracts::WindowId window) {
    if (!window || !ValidSize(viewport_) || dirty_ == Dirty::None) return std::nullopt;
    if (Has(dirty_, Dirty::Layout)) Layout(*root_, {0, 0, viewport_.width, viewport_.height});
    contracts::DisplayList list;
    list.window = window;
    list.generation = ++generation_;
    Paint(*root_, list);
    dirty_ = Dirty::None;
    return list;
}

std::optional<std::string> Scene::Hit(const Node& node, contracts::LogicalPoint point) const {
    if (!Inside(node.bounds, point)) return std::nullopt;
    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        if (auto action = Hit(**it, point)) return action;
    }
    if (!node.action.empty()) return node.action;
    return std::nullopt;
}
std::optional<std::string> Scene::ActionAt(contracts::LogicalPoint point) const {
    return Hit(*root_, point);
}

} // namespace prism::runtime
