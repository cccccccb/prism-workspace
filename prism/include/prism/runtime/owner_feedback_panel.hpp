#pragma once

#include "prism/contracts/owner_feedback.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace prism::runtime {
inline constexpr std::string_view kOwnerFeedbackPanelRegion = "__prism_feedback_panel";
inline constexpr std::string_view kOwnerFeedbackCardRegion = "__prism_feedback_card";
inline constexpr std::string_view kOwnerFeedbackTitleRegion = "__prism_feedback_title_area";
inline constexpr std::string_view kOwnerFeedbackMessageRegion = "__prism_feedback_message_area";
inline constexpr std::array<std::string_view, 2> kOwnerFeedbackLabelRegions{
    "__prism_feedback_label_0", "__prism_feedback_label_1"};

bool IsOwnerFeedbackReservedName(std::string_view name) noexcept;
void ValidateOwnerFeedbackApplication(const Blueprint &app);
Blueprint ComposeOwnerFeedbackPanel(Blueprint app, const Blueprint &shared);
bool HasOwnerFeedbackPanel(const Scene &scene) noexcept;
BindingValues OwnerFeedbackPanelDefaults();
BindingValues OwnerFeedbackPanelBindings(const contracts::OwnerFeedbackRequest &request);
Blueprint InstantiateOwnerFeedbackPanel(const Blueprint &shared,
                                        const contracts::OwnerFeedbackRequest &request,
                                        std::uint64_t generation);
std::string OwnerFeedbackActionName(std::uint64_t generation, std::uint64_t request,
                                    std::uint32_t action);
} // namespace prism::runtime
