#pragma once

#include "prism/contracts/owner_task.hpp"
#include "prism/runtime/blueprint.hpp"
#include "prism/runtime/scene.hpp"

#include <array>
#include <string_view>

namespace prism::runtime {
inline constexpr std::string_view kOwnerTaskPrefix = "__prism_task_";
inline constexpr std::string_view kOwnerTaskPanelRegion = "__prism_task_panel";
inline constexpr std::string_view kOwnerTaskCancelAction = "__prism_task_cancel";
inline constexpr std::array<std::string_view, 2> kOwnerTaskChoiceActions{"__prism_task_choice_0",
                                                                         "__prism_task_choice_1"};
inline constexpr std::array<std::string_view, 4> kOwnerTaskBodyRegions{
    "__prism_task_body", "__prism_task_body_narrow", "__prism_task_body_short",
    "__prism_task_body_narrow_short"};
inline constexpr std::array<std::string_view, 4> kOwnerTaskTitleRegions{
    "__prism_task_title_area", "__prism_task_title_area_narrow", "__prism_task_title_area_short",
    "__prism_task_title_area_narrow_short"};
inline constexpr std::array<std::array<std::string_view, 4>, 2> kOwnerTaskChoiceLabelRegions{
    {{"__prism_task_label_0", "__prism_task_label_0_narrow", "__prism_task_label_0_short",
      "__prism_task_label_0_narrow_short"},
     {"__prism_task_label_1", "__prism_task_label_1_narrow", "__prism_task_label_1_short",
      "__prism_task_label_1_narrow_short"}}};

bool IsOwnerTaskReservedName(std::string_view value) noexcept;
// App declarations cannot enter the Host-owned namespace. Invoke before
// combining an original app/component tree with trusted framework content.
void ValidateOwnerTaskApplication(const Blueprint &app);

// Adds a hidden, mounted overlay immediately before root Popup/Menu declarations.
// Other root kinds and named root regions keep their original tree and capability.
Blueprint ComposeOwnerTaskPanel(Blueprint app, const Blueprint &shared);
bool HasOwnerTaskPanel(const Blueprint &app) noexcept;
bool HasOwnerTaskPanel(const Scene &scene);

BindingValues OwnerTaskPanelDefaults();
BindingValues OwnerTaskPanelBindings(const contracts::OwnerTaskRequest &request);
// Presentation-only wrapping; request bytes remain immutable. LF and all
// non-tab content survive. Tabs display as spaces, including in narrow views.
std::string WrapOwnerTaskText(std::string_view text, double width, double font_size,
                              const ShapeText &shape);
} // namespace prism::runtime
