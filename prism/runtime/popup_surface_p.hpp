#pragma once

#include "prism/runtime/popup_surface.hpp"
#include "prism/runtime/scene_snapshot.hpp"

#include <string>

namespace prism::runtime {
struct PopupVisualPart {
    contracts::NodeId id;
    std::string role;
    bool visible{true};
};

struct PopupControlLayout {
    contracts::NodeId owner;
    double slider_fraction{};
    std::vector<PopupVisualPart> parts;
};

struct PopupSurfaceSource {
    PopupSurfaceRequest metadata; // The source field is empty, avoiding a reference cycle.
    std::shared_ptr<const SceneSnapshot> snapshot;
    std::shared_ptr<const InputSnapshot> input;
    contracts::WindowId window;
    std::vector<PopupControlLayout> controls;
};

struct PopupSurfacePrepared {
    PopupSurfacePlan values; // The prepared field is empty, avoiding a reference cycle.
    PopupSurfaceConfigure configure;
    std::shared_ptr<const SceneSnapshot> layout; // Resolved parent-space geometry.
    bool layout_reused{};
};

void ApplyPopupControlVisuals(SceneSnapshot &, const PopupSurfaceSource &);
std::shared_ptr<const InputSnapshot> PreparePopupInput(const SceneSnapshot &,
                                                       const PopupSurfaceSource &,
                                                       contracts::LogicalSize viewport);
std::vector<contracts::SurfaceInputRegion>
PreparePopupInputRegions(const SceneSnapshot &, contracts::NodeId root,
                         contracts::LogicalSize viewport);
std::vector<contracts::SurfaceEffectRegion>
PreparePopupEffects(const SceneSnapshot &, contracts::NodeId root, contracts::LogicalSize viewport);

PopupSurfacePlan PreparePopupSurfaceValues(const PopupSurfaceRequest &,
                                           const PopupSurfaceConfigure &, const ShapeText &,
                                           contracts::ResourceId font,
                                           const PopupSurfacePrepared *previous = nullptr);
bool ReusePopupLayout(SceneSnapshot &, const PopupSurfaceRequest &, const PopupSurfaceConfigure &,
                      contracts::LogicalRect body, const PopupSurfacePrepared *);
} // namespace prism::runtime
