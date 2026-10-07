#include "prism/runtime/owner_feedback_panel.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
void ValidateAction(const PropertyAssignment &property)
{
    if (property.id != DslProperty::Action && property.id != DslProperty::PopupFor &&
        property.id != DslProperty::TooltipFor) {
        return;
    }
    if (const auto *value = std::get_if<std::string>(&property.value);
        value && IsOwnerFeedbackReservedName(*value)) {
        throw std::invalid_argument("Application action uses the reserved feedback namespace");
    }
}

void ValidateShared(const Blueprint &node, std::size_t &count, std::size_t depth = 1)
{
    if (++count > 8192 || depth > 64 || !node.region.empty() || IsFloatingKind(node.kind) ||
        node.kind == Kind::Image || node.gesture) {
        throw std::invalid_argument("Feedback panel requires a bounded resource-free tree");
    }
    for (const auto &binding : node.bindings) {
        if (!IsOwnerFeedbackReservedName(binding.name)) {
            throw std::invalid_argument("Feedback bindings must belong to the Host namespace");
        }
    }
    for (const auto &property : node.properties) {
        if (property.id == DslProperty::Source || property.id == DslProperty::PopupFor) {
            throw std::invalid_argument("Feedback panel cannot reference external resources");
        }
        if (property.id == DslProperty::Action) {
            const auto *action = std::get_if<std::string>(&property.value);
            if (!action ||
                (*action != "__prism_feedback_dismiss" && *action != "__prism_feedback_choice_0" &&
                 *action != "__prism_feedback_choice_1")) {
                throw std::invalid_argument("Unexpected shared feedback action");
            }
        }
    }
    for (const auto &child : node.children) {
        ValidateShared(child, count, depth + 1);
    }
}

void PrepareTree(Blueprint &node, const contracts::OwnerFeedbackRequest *request,
                 std::uint64_t generation, std::size_t &measurements, std::size_t &cards)
{
    for (const auto &binding : node.bindings) {
        if (binding.name != "__prism_feedback_card_visible") {
            continue;
        }
        if (binding.target != DslProperty::Visible || node.kind != Kind::Box) {
            throw std::invalid_argument("Feedback card marker requires a Card visibility binding");
        }
        node.region = kOwnerFeedbackCardRegion;
        node.region_mounted = true;
        ++cards;
    }
    for (auto &property : node.properties) {
        if (request && property.id == DslProperty::Action) {
            auto &action = std::get<std::string>(property.value);
            std::uint32_t id = 0;
            if (action == "__prism_feedback_choice_0" && !request->actions.empty()) {
                id = request->actions[0].id;
            } else if (action == "__prism_feedback_choice_1" && request->actions.size() == 2) {
                id = request->actions[1].id;
            }
            action = OwnerFeedbackActionName(generation, request->request_id, id);
        }
    }
    if (node.kind == Kind::Text) {
        for (const auto &binding : node.bindings) {
            std::string_view region;
            if (binding.name == "__prism_feedback_title") {
                region = kOwnerFeedbackTitleRegion;
            } else if (binding.name == "__prism_feedback_message") {
                region = kOwnerFeedbackMessageRegion;
            } else if (binding.name == "__prism_feedback_choice_0_label") {
                region = kOwnerFeedbackLabelRegions[0];
            } else if (binding.name == "__prism_feedback_choice_1_label") {
                region = kOwnerFeedbackLabelRegions[1];
            }
            if (region.empty()) {
                continue;
            }
            Blueprint wrapper;
            wrapper.region = region;
            wrapper.region_mounted = true;
            wrapper.children.push_back(std::move(node));
            node = std::move(wrapper);
            ++measurements;
            return;
        }
    }
    for (auto &child : node.children) {
        PrepareTree(child, request, generation, measurements, cards);
    }
}

void ValidateBudget(const Blueprint &node, std::size_t &count, std::size_t depth = 1)
{
    if (++count > 8192 || depth > 64) {
        throw std::length_error("Feedback composition exceeds Scene limits");
    }
    for (const auto &child : node.children) {
        ValidateBudget(child, count, depth + 1);
    }
}

Blueprint PrepareShared(const Blueprint &shared, const contracts::OwnerFeedbackRequest *request,
                        std::uint64_t generation)
{
    if (shared.kind != Kind::Box) {
        throw std::invalid_argument("Feedback panel requires a Card root");
    }
    std::size_t count = 0;
    ValidateShared(shared, count);
    auto tree = shared;
    std::size_t measurements = 0;
    std::size_t cards = 0;
    PrepareTree(tree, request, generation, measurements, cards);
    if (measurements != 4 || cards != 1) {
        throw std::invalid_argument("Feedback panel requires title, body, labels and one card");
    }
    return tree;
}
} // namespace

bool IsOwnerFeedbackReservedName(std::string_view name) noexcept
{
    return name.starts_with("__prism_feedback_");
}

void ValidateOwnerFeedbackApplication(const Blueprint &app)
{
    if (IsOwnerFeedbackReservedName(app.region)) {
        throw std::invalid_argument("Application region uses the reserved feedback namespace");
    }
    for (const auto &binding : app.bindings) {
        if (IsOwnerFeedbackReservedName(binding.name)) {
            throw std::invalid_argument("Application binding uses the reserved feedback namespace");
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
    if (app.gesture && IsOwnerFeedbackReservedName(app.gesture->action)) {
        throw std::invalid_argument("Application gesture uses the reserved feedback namespace");
    }
    for (const auto &child : app.children) {
        ValidateOwnerFeedbackApplication(child);
    }
}

Blueprint ComposeOwnerFeedbackPanel(Blueprint app, const Blueprint &shared)
{
    ValidateOwnerFeedbackApplication(app);
    if (app.kind != Kind::Box || !app.region.empty()) {
        return app;
    }
    Blueprint wrapper;
    wrapper.region = kOwnerFeedbackPanelRegion;
    wrapper.region_mounted = true;
    wrapper.properties.push_back({DslProperty::Visible, false});
    wrapper.bindings.push_back({"__prism_feedback_visible", DslProperty::Visible});
    wrapper.children.push_back(PrepareShared(shared, nullptr, 0));
    const auto popup =
        std::find_if(app.children.begin(), app.children.end(),
                     [](const Blueprint &child) { return IsFloatingKind(child.kind); });
    app.children.insert(popup, std::move(wrapper));
    std::size_t count = 0;
    ValidateBudget(app, count);
    return app;
}

bool HasOwnerFeedbackPanel(const Scene &scene) noexcept
{
    try {
        return scene.RegionId(kOwnerFeedbackPanelRegion) &&
               scene.RegionMounted(kOwnerFeedbackPanelRegion);
    } catch (...) {
        return false;
    }
}

BindingValues OwnerFeedbackPanelDefaults()
{
    return {{"__prism_feedback_visible", false},
            {"__prism_feedback_card_visible", true},
            {"__prism_feedback_title", std::string{}},
            {"__prism_feedback_message", std::string{}},
            {"__prism_feedback_info", true},
            {"__prism_feedback_success", false},
            {"__prism_feedback_error", false},
            {"__prism_feedback_has_actions", false},
            {"__prism_feedback_choice_0_visible", false},
            {"__prism_feedback_choice_1_visible", false},
            {"__prism_feedback_choice_0_label", std::string{}},
            {"__prism_feedback_choice_1_label", std::string{}}};
}

BindingValues OwnerFeedbackPanelBindings(const contracts::OwnerFeedbackRequest &request)
{
    if (!contracts::ValidateOwnerFeedbackRequest(request)) {
        throw std::invalid_argument("Invalid feedback request");
    }
    auto values = OwnerFeedbackPanelDefaults();
    values.insert_or_assign("__prism_feedback_visible", true);
    values.insert_or_assign("__prism_feedback_title", request.title);
    values.insert_or_assign("__prism_feedback_message", request.message);
    values.insert_or_assign("__prism_feedback_info",
                            request.kind == contracts::OwnerFeedbackKind::Info);
    values.insert_or_assign("__prism_feedback_success",
                            request.kind == contracts::OwnerFeedbackKind::Success);
    values.insert_or_assign("__prism_feedback_error",
                            request.kind == contracts::OwnerFeedbackKind::Error);
    values.insert_or_assign("__prism_feedback_has_actions", !request.actions.empty());
    for (std::size_t i = 0; i < request.actions.size(); ++i) {
        const auto prefix = "__prism_feedback_choice_" + std::to_string(i);
        values.insert_or_assign(prefix + "_visible", true);
        values.insert_or_assign(prefix + "_label", request.actions[i].label);
    }
    return values;
}

Blueprint InstantiateOwnerFeedbackPanel(const Blueprint &shared,
                                        const contracts::OwnerFeedbackRequest &request,
                                        std::uint64_t generation)
{
    if (!generation || !contracts::ValidateOwnerFeedbackRequest(request)) {
        throw std::invalid_argument("Invalid feedback generation or request");
    }
    return PrepareShared(shared, &request, generation);
}

std::string OwnerFeedbackActionName(std::uint64_t generation, std::uint64_t request,
                                    std::uint32_t action)
{
    return "__prism_feedback_event_" + std::to_string(generation) + "_" + std::to_string(request) +
           "_" + std::to_string(action);
}
} // namespace prism::runtime
