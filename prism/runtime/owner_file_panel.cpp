#include "prism/runtime/owner_file_panel.hpp"
#include "prism/runtime/text_buffer.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
bool ValidText(std::string_view text, std::size_t limit) noexcept
{
    return text.size() <= limit && TextBuffer::Valid(text) &&
           text.find('\r') == std::string_view::npos;
}

bool FileAction(std::string_view action)
{
    constexpr std::array actions{
        kOwnerTaskCancelAction,   kOwnerFileUpAction,      kOwnerFileHomeAction,
        kOwnerFilePreviousAction, kOwnerFileNextAction,    kOwnerFileSubmitAction,
        kOwnerFileNameAction,     kOwnerFileReplaceAction, kOwnerFileBackAction};
    return std::find(actions.begin(), actions.end(), action) != actions.end() ||
           std::find(kOwnerFileRowActions.begin(), kOwnerFileRowActions.end(), action) !=
               kOwnerFileRowActions.end();
}

void ValidateFileTree(const Blueprint &node, const BindingValues &bindings, std::size_t &count,
                      std::size_t depth)
{
    if (++count > 2048 || depth > 32 || !node.region.empty() || IsFloatingKind(node.kind) ||
        node.kind == Kind::Image || node.gesture) {
        throw std::invalid_argument("File panel requires a bounded resource-free ordinary tree");
    }
    for (const auto &binding : node.bindings) {
        if (!bindings.contains(binding.name)) {
            throw std::invalid_argument("Unknown shared file panel binding");
        }
    }
    for (const auto &property : node.properties) {
        if (property.id == DslProperty::Source || property.id == DslProperty::PopupFor) {
            throw std::invalid_argument("File panel cannot use app resources or popups");
        }
        if (property.id == DslProperty::Action) {
            const auto *action = std::get_if<std::string>(&property.value);
            if (!action || !FileAction(*action) ||
                (*action == kOwnerFileNameAction && node.kind != Kind::TextField)) {
                throw std::invalid_argument("Unexpected shared file panel action");
            }
        }
    }
    for (const auto &state : node.state_rules) {
        for (const auto &property : state.properties) {
            if (property.id == DslProperty::Action || property.id == DslProperty::Source ||
                property.id == DslProperty::PopupFor) {
                throw std::invalid_argument("File panel states cannot change action identity");
            }
        }
    }
    for (const auto &child : node.children) {
        ValidateFileTree(child, bindings, count, depth + 1);
    }
}

struct MeasurementCounts {
    std::size_t details{}, titles{}, status{};
};

void AddMeasurementRegions(Blueprint &node, MeasurementCounts &counts, bool decorative = false)
{
    decorative = decorative || node.kind == Kind::Visual;
    if (node.kind == Kind::Text && !decorative) {
        for (const auto &binding : node.bindings) {
            if (binding.target != DslProperty::Text) {
                continue;
            }
            std::string_view region;
            if (binding.name == "__prism_task_file_selected_caption") {
                if (counts.details >= kOwnerFileDetailRegions.size()) {
                    throw std::invalid_argument("Too many file panel detail views");
                }
                region = kOwnerFileDetailRegions[counts.details++];
            } else if (binding.name == "__prism_task_file_title") {
                if (counts.titles >= kOwnerFileTitleRegions.size()) {
                    throw std::invalid_argument("Too many file panel title views");
                }
                region = kOwnerFileTitleRegions[counts.titles++];
            } else if (binding.name == "__prism_task_file_status") {
                if (counts.status >= kOwnerFileStatusRegions.size()) {
                    throw std::invalid_argument("Too many file panel status views");
                }
                region = kOwnerFileStatusRegions[counts.status++];
            }
            if (region.empty()) {
                continue;
            }

            Blueprint wrapper;
            wrapper.region = region;
            wrapper.region_mounted = true;
            wrapper.children.push_back(std::move(node));
            node = std::move(wrapper);
            return;
        }
    }
    for (auto &child : node.children) {
        AddMeasurementRegions(child, counts, decorative);
    }
}

void SetMode(Blueprint &tree, const std::string &binding)
{
    std::erase_if(tree.properties, [](const PropertyAssignment &property) {
        return property.id == DslProperty::Visible;
    });
    std::erase_if(tree.bindings, [](const PropertyBinding &entry) {
        return entry.target == DslProperty::Visible;
    });
    tree.bindings.push_back({binding, DslProperty::Visible});
}

void ValidateCombinedBudget(const Blueprint &node, std::size_t &count, std::size_t depth)
{
    if (++count > 8192 || depth > 64) {
        throw std::length_error("File panel composition exceeds the Scene node or depth limit");
    }
    for (const auto &child : node.children) {
        ValidateCombinedBudget(child, count, depth + 1);
    }
}
} // namespace

bool ValidateOwnerFilePanelView(const OwnerFilePanelView &view) noexcept
{
    if (!ValidText(view.title, contracts::kMaxOwnerTaskTitleBytes) ||
        !ValidText(view.directory, contracts::kMaxOwnerFileTaskPathBytes) ||
        !ValidText(view.filename, contracts::kMaxOwnerFileTaskPathBytes) ||
        !ValidText(view.status, contracts::kMaxOwnerTaskDiagnosticBytes) ||
        !ValidText(view.page_caption, 96) ||
        !ValidText(view.selected_caption, contracts::kMaxOwnerFileTaskPathBytes) ||
        (view.overwrite && !view.show_filename)) {
        return false;
    }
    for (const auto &row : view.rows) {
        if (!ValidText(row.name, contracts::kMaxOwnerFileTaskNameBytes) ||
            (row.name.empty() && (row.directory || row.selected))) {
            return false;
        }
    }
    return true;
}

BindingValues OwnerFilePanelDefaults()
{
    BindingValues values{{"__prism_task_visible", false},
                         {"__prism_task_confirmation_mode", false},
                         {"__prism_task_file_mode", false},
                         {"__prism_task_file_title", std::string{}},
                         {"__prism_task_file_directory", std::string{}},
                         {"__prism_task_file_filename", std::string{}},
                         {"__prism_task_file_status", std::string{}},
                         {"__prism_task_file_page_caption", std::string{}},
                         {"__prism_task_file_selected_caption", std::string{}},
                         {"__prism_task_file_submit_caption", std::string{"Open"}},
                         {"__prism_task_file_nav_enabled", false},
                         {"__prism_task_file_submit_enabled", false},
                         {"__prism_task_file_editing_enabled", false},
                         {"__prism_task_file_loading", false},
                         {"__prism_task_file_overwrite", false},
                         {"__prism_task_file_normal", true},
                         {"__prism_task_file_show_filename", false}};
    for (std::size_t index = 0; index < kOwnerFilePanelRows; ++index) {
        const auto prefix = std::string(kOwnerFileRowActions[index]);
        values.emplace(prefix + "_name", std::string{});
        values.emplace(prefix + "_visible", false);
        values.emplace(prefix + "_directory", false);
        values.emplace(prefix + "_file", true);
        values.emplace(prefix + "_selected", false);
    }
    return values;
}

BindingValues OwnerFilePanelBindings(contracts::OwnerTaskKind kind, const OwnerFilePanelView &view)
{
    if (kind == contracts::OwnerTaskKind::Confirmation || !contracts::OwnerTaskCapability(kind) ||
        !ValidateOwnerFilePanelView(view) ||
        view.show_filename != (kind == contracts::OwnerTaskKind::SaveFile)) {
        throw std::invalid_argument("Invalid file panel presentation");
    }

    auto values = OwnerFilePanelDefaults();
    values.insert_or_assign("__prism_task_visible", true);
    values.insert_or_assign("__prism_task_file_mode", true);
    values.insert_or_assign("__prism_task_file_title", view.title);
    values.insert_or_assign("__prism_task_file_directory", view.directory);
    values.insert_or_assign("__prism_task_file_filename", view.filename);
    values.insert_or_assign("__prism_task_file_status", view.status);
    values.insert_or_assign("__prism_task_file_page_caption", view.page_caption);
    values.insert_or_assign("__prism_task_file_selected_caption", view.selected_caption);
    values.insert_or_assign("__prism_task_file_nav_enabled",
                            view.nav_enabled && !view.overwrite && !view.loading);
    values.insert_or_assign("__prism_task_file_submit_enabled",
                            view.submit_enabled && !view.loading);
    values.insert_or_assign("__prism_task_file_editing_enabled", !view.loading && !view.overwrite);
    values.insert_or_assign("__prism_task_file_loading", view.loading);
    values.insert_or_assign("__prism_task_file_overwrite", view.overwrite);
    values.insert_or_assign("__prism_task_file_normal", !view.overwrite);
    values.insert_or_assign("__prism_task_file_show_filename", view.show_filename);
    values.insert_or_assign("__prism_task_file_submit_caption",
                            std::string(kind == contracts::OwnerTaskKind::SaveFile ? "Save"
                                        : kind == contracts::OwnerTaskKind::SelectDirectory
                                            ? "Choose"
                                            : "Open"));
    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        const auto prefix = std::string(kOwnerFileRowActions[index]);
        const auto &row = view.rows[index];
        values.insert_or_assign(prefix + "_name", row.name);
        values.insert_or_assign(prefix + "_visible", !row.name.empty());
        values.insert_or_assign(prefix + "_directory", row.directory);
        values.insert_or_assign(prefix + "_file", !row.directory);
        values.insert_or_assign(prefix + "_selected", row.selected);
    }
    return values;
}

Blueprint ComposeOwnerTaskPanels(Blueprint app, const Blueprint *confirmation,
                                 const Blueprint *file)
{
    ValidateOwnerTaskApplication(app);
    if ((!confirmation && !file) || app.kind != Kind::Box || !app.region.empty()) {
        return app;
    }
    if (confirmation) {
        app = ComposeOwnerTaskPanel(std::move(app), *confirmation);
    }
    if (!file) {
        return app;
    }
    if (file->kind != Kind::Box) {
        throw std::invalid_argument("Shared file panel requires a Card root");
    }
    auto defaults = OwnerFilePanelDefaults();
    std::size_t nodes = 0;
    ValidateFileTree(*file, defaults, nodes, 1);

    Blueprint content;
    if (confirmation) {
        auto wrapper =
            std::find_if(app.children.begin(), app.children.end(), [](const Blueprint &child) {
                return child.region == kOwnerTaskPanelRegion;
            });
        content.children.push_back(std::move(wrapper->children.front()));
        SetMode(content.children.back(), "__prism_task_confirmation_mode");
        wrapper->children.clear();
    }
    content.children.push_back(*file);
    SetMode(content.children.back(), "__prism_task_file_mode");
    MeasurementCounts counts;
    AddMeasurementRegions(content.children.back(), counts);
    if (!counts.details || !counts.titles || !counts.status) {
        throw std::invalid_argument("File panel requires readable title, status and detail views");
    }

    if (confirmation) {
        auto wrapper =
            std::find_if(app.children.begin(), app.children.end(), [](const Blueprint &child) {
                return child.region == kOwnerTaskPanelRegion;
            });
        wrapper->children.push_back(std::move(content));
    } else {
        Blueprint wrapper;
        wrapper.region = kOwnerTaskPanelRegion;
        wrapper.region_mounted = true;
        wrapper.properties.push_back({DslProperty::Visible, false});
        wrapper.bindings.push_back({"__prism_task_visible", DslProperty::Visible});
        wrapper.children.push_back(std::move(content));
        const auto popup =
            std::find_if(app.children.begin(), app.children.end(),
                         [](const Blueprint &child) { return IsFloatingKind(child.kind); });
        app.children.insert(popup, std::move(wrapper));
    }
    nodes = 0;
    ValidateCombinedBudget(app, nodes, 1);
    return app;
}
} // namespace prism::runtime
