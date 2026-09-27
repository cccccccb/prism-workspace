#pragma once

#include "prism/runtime/scene.hpp"
#include <chrono>
#include <cstddef>
#include <memory>

namespace prism::runtime {
// Owner-thread node allocation is cooperative. Final shaping/layout validation
// belongs to Scene::Preflight and remains an indivisible work unit.
class SceneConstruction {
public:
    SceneConstruction(Blueprint, ShapeText, contracts::ResourceId font = {},
                      std::optional<contracts::ThemeSnapshot> theme = {});
    ~SceneConstruction();
    SceneConstruction(const SceneConstruction &) = delete;
    SceneConstruction &operator=(const SceneConstruction &) = delete;
    bool Advance(std::size_t max_nodes, std::chrono::steady_clock::time_point deadline);
    bool Ready() const noexcept;
    std::unique_ptr<Scene> TakeScene();
    std::size_t ConstructedNodes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::runtime
