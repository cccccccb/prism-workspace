#include "prism/runtime/scene_construction.hpp"
#include "scene_p.hpp"
#include <stdexcept>
#include <utility>

namespace prism::runtime {
struct SceneConstruction::Impl {
    struct Pending {
        Blueprint blueprint;
        Scene::Node *parent{};
        std::size_t depth{1};
    };

    std::unique_ptr<Scene> scene;
    std::vector<Pending> pending;
    std::size_t count{};
    bool consumed{false};
    bool failed{false};
};

SceneConstruction::SceneConstruction(Blueprint root, ShapeText shaper, contracts::ResourceId font,
                                     std::optional<contracts::ThemeSnapshot> theme)
    : impl_(std::make_unique<Impl>())
{
    impl_->scene.reset(
        new Scene(Scene::EmptyConstruction{}, std::move(shaper), font, std::move(theme)));
    impl_->pending.push_back({std::move(root), nullptr, 1});
}

SceneConstruction::~SceneConstruction() = default;

bool SceneConstruction::Advance(std::size_t max_nodes,
                                std::chrono::steady_clock::time_point deadline)
{
    if (!max_nodes || impl_->consumed || impl_->failed) {
        throw std::logic_error("Invalid SceneConstruction advance");
    }
    try {
        std::size_t advanced = 0;
        while (!impl_->pending.empty() && advanced < max_nodes &&
               std::chrono::steady_clock::now() < deadline) {
            auto current = std::move(impl_->pending.back());
            impl_->pending.pop_back();
            auto children = std::move(current.blueprint.children);
            auto node = impl_->scene->MakeShallowNode(std::move(current.blueprint), current.depth);
            auto *raw = node.get();
            raw->parent = current.parent;
            raw->children.reserve(children.size());
            if (current.parent) {
                current.parent->children.push_back(std::move(node));
            } else {
                impl_->scene->root_ = std::move(node);
            }
            for (auto child = children.rbegin(); child != children.rend(); ++child) {
                impl_->pending.push_back({std::move(*child), raw, current.depth + 1});
            }
            ++impl_->count;
            ++advanced;
        }
    } catch (...) {
        impl_->failed = true;
        throw;
    }
    return Ready();
}

bool SceneConstruction::Ready() const noexcept
{
    return !impl_->consumed && !impl_->failed && impl_->pending.empty();
}

std::unique_ptr<Scene> SceneConstruction::TakeScene()
{
    if (!Ready()) {
        throw std::logic_error("Scene construction is not ready");
    }
    impl_->consumed = true;
    return std::move(impl_->scene);
}

std::size_t SceneConstruction::ConstructedNodes() const noexcept
{
    return impl_->count;
}
} // namespace prism::runtime
