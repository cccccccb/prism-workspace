#include "prism/runtime/owner_task_panel.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace prism::runtime {
namespace {
void ValidateAction(const PropertyAssignment &assignment)
{
    if (assignment.id != DslProperty::Action && assignment.id != DslProperty::PopupFor) {
        return;
    }
    if (const auto *text = std::get_if<std::string>(&assignment.value);
        text && IsOwnerTaskReservedName(*text)) {
        throw std::invalid_argument("Application action uses the reserved owner task namespace");
    }
}

void ValidateSharedPanel(const Blueprint &node)
{
    if (!node.region.empty() || IsPopupKind(node.kind) || node.kind == Kind::Image) {
        throw std::invalid_argument("Shared task panel requires an ordinary resource-free tree");
    }
    for (const auto &binding : node.bindings) {
        if (!IsOwnerTaskReservedName(binding.name)) {
            throw std::invalid_argument("Shared task bindings must remain in the Host namespace");
        }
    }
    for (const auto &property : node.properties) {
        if (property.id == DslProperty::Source) {
            throw std::invalid_argument("Shared task panel cannot reference app image resources");
        }
        if (property.id == DslProperty::Action) {
            const auto *action = std::get_if<std::string>(&property.value);
            if (!action ||
                (*action != kOwnerTaskCancelAction &&
                 std::find(kOwnerTaskChoiceActions.begin(), kOwnerTaskChoiceActions.end(),
                           *action) == kOwnerTaskChoiceActions.end())) {
                throw std::invalid_argument("Unexpected shared task action");
            }
        }
    }
    for (const auto &child : node.children) {
        ValidateSharedPanel(child);
    }
}

void ValidateCombinedBudget(const Blueprint &node, std::size_t &count, std::size_t depth)
{
    if (++count > 8192 || depth > 64) {
        throw std::length_error("Task panel composition exceeds the Scene node or depth limit");
    }
    for (const auto &child : node.children) {
        ValidateCombinedBudget(child, count, depth + 1);
    }
}

struct MeasurementCounts {
    std::size_t messages{}, titles{};
    std::array<std::size_t, 2> labels{};
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
            if (binding.name == "__prism_task_message") {
                if (counts.messages == kOwnerTaskBodyRegions.size()) {
                    throw std::invalid_argument("Too many shared task message views");
                }
                region = kOwnerTaskBodyRegions[counts.messages++];
            } else if (binding.name == "__prism_task_title") {
                if (counts.titles == kOwnerTaskTitleRegions.size()) {
                    throw std::invalid_argument("Too many shared task title views");
                }
                region = kOwnerTaskTitleRegions[counts.titles++];
            } else {
                for (std::size_t index = 0; index < counts.labels.size(); ++index) {
                    if (binding.name != std::string(kOwnerTaskChoiceActions[index]) + "_label") {
                        continue;
                    }
                    if (counts.labels[index] == kOwnerTaskChoiceLabelRegions[index].size()) {
                        throw std::invalid_argument("Too many shared task choice label views");
                    }
                    region = kOwnerTaskChoiceLabelRegions[index][counts.labels[index]++];
                }
            }
            if (region.empty()) {
                continue;
            }

            Blueprint wrapper;
            wrapper.region = region;
            wrapper.region_mounted = true;
            if (binding.name == "__prism_task_choice_0_label" ||
                binding.name == "__prism_task_choice_1_label") {
                wrapper.properties.push_back({DslProperty::Justify, std::string("center")});
            }
            wrapper.children.push_back(std::move(node));
            node = std::move(wrapper);
            return;
        }
    }
    for (auto &child : node.children) {
        AddMeasurementRegions(child, counts, decorative);
    }
}
} // namespace

bool IsOwnerTaskReservedName(std::string_view value) noexcept
{
    return value.starts_with(kOwnerTaskPrefix);
}

void ValidateOwnerTaskApplication(const Blueprint &app)
{
    if (IsOwnerTaskReservedName(app.region)) {
        throw std::invalid_argument("Application region uses the reserved owner task namespace");
    }
    for (const auto &binding : app.bindings) {
        if (IsOwnerTaskReservedName(binding.name)) {
            throw std::invalid_argument(
                "Application binding uses the reserved owner task namespace");
        }
    }
    for (const auto &property : app.properties) {
        ValidateAction(property);
    }
    for (const auto &state : app.state_rules) {
        for (const auto &property : state.properties) {
            ValidateAction(property);
        }
    }
    if (app.gesture && IsOwnerTaskReservedName(app.gesture->action)) {
        throw std::invalid_argument("Application gesture uses the reserved owner task namespace");
    }
    for (const auto &child : app.children) {
        ValidateOwnerTaskApplication(child);
    }
}

Blueprint ComposeOwnerTaskPanel(Blueprint app, const Blueprint &shared)
{
    ValidateOwnerTaskApplication(app);
    if (app.kind != Kind::Box || !app.region.empty()) {
        return app;
    }
    if (shared.kind != Kind::Box) {
        throw std::invalid_argument("Shared task panel requires a Card root");
    }
    ValidateSharedPanel(shared);

    Blueprint wrapper;
    wrapper.kind = Kind::Box;
    wrapper.region = kOwnerTaskPanelRegion;
    wrapper.region_mounted = true;
    wrapper.properties.push_back({DslProperty::Visible, false});
    wrapper.bindings.push_back({"__prism_task_visible", DslProperty::Visible});
    wrapper.children.push_back(shared);
    MeasurementCounts counts;
    AddMeasurementRegions(wrapper.children.front(), counts);
    if (!counts.messages || !counts.titles || !counts.labels[0] || !counts.labels[1]) {
        throw std::invalid_argument("Shared task panel requires title, message and choice views");
    }
    const auto popup = std::find_if(app.children.begin(), app.children.end(),
                                    [](const Blueprint &child) { return IsPopupKind(child.kind); });
    app.children.insert(popup, std::move(wrapper));

    std::size_t count = 0;
    ValidateCombinedBudget(app, count, 1);
    return app;
}

bool HasOwnerTaskPanel(const Blueprint &app) noexcept
{
    if (app.kind != Kind::Box || !app.region.empty()) {
        return false;
    }
    for (const auto &child : app.children) {
        if (child.kind == Kind::Box && child.region == kOwnerTaskPanelRegion &&
            child.region_mounted && child.children.size() == 1) {
            return true;
        }
    }
    return false;
}

bool HasOwnerTaskPanel(const Scene &scene)
{
    return scene.RegionId(kOwnerTaskPanelRegion) && scene.RegionMounted(kOwnerTaskPanelRegion);
}

BindingValues OwnerTaskPanelDefaults()
{
    return {{"__prism_task_visible", false},
            {"__prism_task_confirmation_mode", false},
            {"__prism_task_file_mode", false},
            {"__prism_task_context", std::string{"Application request"}},
            {"__prism_task_title", std::string{}},
            {"__prism_task_message", std::string{}},
            {"__prism_task_choice_0_label", std::string{}},
            {"__prism_task_choice_1_label", std::string{}},
            {"__prism_task_choice_1_visible", false},
            {"__prism_task_choice_0_primary", false},
            {"__prism_task_choice_1_primary", false},
            {"__prism_task_choice_0_destructive", false},
            {"__prism_task_choice_1_destructive", false},
            {"__prism_task_narrow_actions_height", 72.0}};
}

BindingValues OwnerTaskPanelBindings(const contracts::OwnerTaskRequest &request)
{
    if (request.kind != contracts::OwnerTaskKind::Confirmation ||
        !contracts::ValidateOwnerTaskRequest(request)) {
        throw std::invalid_argument("Invalid confirmation panel request");
    }

    auto bindings = OwnerTaskPanelDefaults();
    bindings.insert_or_assign("__prism_task_visible", true);
    bindings.insert_or_assign("__prism_task_confirmation_mode", true);
    bindings.insert_or_assign("__prism_task_title", request.title);
    bindings.insert_or_assign("__prism_task_message", request.message);
    bindings.insert_or_assign("__prism_task_choice_1_visible", request.choices.size() == 2);
    bindings.insert_or_assign("__prism_task_narrow_actions_height",
                              request.choices.size() == 2 ? 112.0 : 72.0);
    for (std::size_t index = 0; index < request.choices.size(); ++index) {
        const auto prefix = std::string(kOwnerTaskChoiceActions[index]);
        const auto &choice = request.choices[index];
        bindings.insert_or_assign(prefix + "_label", choice.label);
        bindings.insert_or_assign(prefix + "_primary",
                                  choice.role == contracts::OwnerTaskChoiceRole::Primary);
        bindings.insert_or_assign(prefix + "_destructive",
                                  choice.role == contracts::OwnerTaskChoiceRole::Destructive);
    }
    return bindings;
}
} // namespace prism::runtime
