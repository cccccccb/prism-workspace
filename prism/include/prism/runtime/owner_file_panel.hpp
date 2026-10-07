#pragma once

#include "prism/runtime/owner_task_panel.hpp"

#include <array>
#include <string>
#include <string_view>

namespace prism::runtime {
inline constexpr std::size_t kOwnerFilePanelRows = 8;
inline constexpr std::array<std::string_view, kOwnerFilePanelRows> kOwnerFileRowActions{
    "__prism_task_file_row_0", "__prism_task_file_row_1", "__prism_task_file_row_2",
    "__prism_task_file_row_3", "__prism_task_file_row_4", "__prism_task_file_row_5",
    "__prism_task_file_row_6", "__prism_task_file_row_7"};
inline constexpr std::string_view kOwnerFileUpAction = "__prism_task_file_up";
inline constexpr std::string_view kOwnerFileHomeAction = "__prism_task_file_home";
inline constexpr std::string_view kOwnerFilePreviousAction = "__prism_task_file_previous";
inline constexpr std::string_view kOwnerFileNextAction = "__prism_task_file_next";
inline constexpr std::string_view kOwnerFileSubmitAction = "__prism_task_file_submit";
inline constexpr std::string_view kOwnerFileNameAction = "__prism_task_file_name";
inline constexpr std::string_view kOwnerFileReplaceAction = "__prism_task_file_replace";
inline constexpr std::string_view kOwnerFileBackAction = "__prism_task_file_back";
inline constexpr std::array<std::string_view, 3> kOwnerFileDetailRegions{
    "__prism_task_file_detail", "__prism_task_file_detail_compact",
    "__prism_task_file_detail_short"};
inline constexpr std::array<std::string_view, 2> kOwnerFileTitleRegions{
    "__prism_task_file_title_area", "__prism_task_file_title_area_short"};
inline constexpr std::array<std::string_view, 2> kOwnerFileStatusRegions{
    "__prism_task_file_status_area", "__prism_task_file_status_area_short"};

struct OwnerFilePanelRow {
    std::string name;
    bool directory{};
    bool selected{};
    bool operator==(const OwnerFilePanelRow &) const = default;
};

// Owned presentation values only. Empty row names hide unused fixed slots.
// Full selection text stays available through the wrapped detail ScrollView.
// Filename editing permits invalid names up to 4096 bytes; validation before
// submission remains the provider's responsibility.
struct OwnerFilePanelView {
    std::array<OwnerFilePanelRow, kOwnerFilePanelRows> rows;
    std::string title;
    std::string directory;
    std::string filename;
    std::string status;
    std::string page_caption;
    std::string selected_caption;
    bool nav_enabled{};
    bool submit_enabled{};
    bool loading{};
    bool overwrite{};
    bool show_filename{};
    bool operator==(const OwnerFilePanelView &) const = default;
};

bool ValidateOwnerFilePanelView(const OwnerFilePanelView &view) noexcept;
BindingValues OwnerFilePanelDefaults();
BindingValues OwnerFilePanelBindings(contracts::OwnerTaskKind kind, const OwnerFilePanelView &view);
// The combined overlay keeps a single mounted scope before root Popup/Menu.
Blueprint ComposeOwnerTaskPanels(Blueprint app, const Blueprint *confirmation,
                                 const Blueprint *file);
} // namespace prism::runtime
