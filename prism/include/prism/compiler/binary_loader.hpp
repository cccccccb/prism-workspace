#pragma once

#include "prism/scene/node.hpp"
#include <memory>
#include <string>

namespace prism::decoration {
class TilingDecorationSpec;
}

namespace prism::compiler {

/**
 * @brief Zero-copy AOT Binary Scene Graph Loader:
 *        Uses memory mapping (mmap) to reconstruct the SceneNode composite tree in < 0.05ms
 */
class BinarySceneLoader {
public:
    static std::shared_ptr<scene::SceneNode> LoadFromFile(const std::string &prismb_path);
    static std::shared_ptr<scene::SceneNode> LoadFromPackage(const std::string &pkg_path,
                                                             const std::string &internal_file_name);
    static std::shared_ptr<scene::SceneNode> LoadFromMemory(const uint8_t *data, size_t size);
};

/**
 * @brief Zero-copy AOT Binary Theme Loader:
 *        Loads compiled .prismb theme files into TilingDecorationSpec in < 1 microsecond.
 */
class BinaryThemeLoader {
public:
    static std::shared_ptr<decoration::TilingDecorationSpec>
    LoadFromFile(const std::string &prismb_path);
    static std::shared_ptr<decoration::TilingDecorationSpec> LoadFromMemory(const uint8_t *data,
                                                                            size_t size);
};

} // namespace prism::compiler
