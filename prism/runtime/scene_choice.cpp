#include "scene_p.hpp"
#include <stdexcept>

namespace prism::runtime {
std::string_view Scene::InputAction(const Node &node) const
{
    if (node.kind == Kind::MenuBack) {
        return "$prism.menu.back";
    }
    if (IsChoiceGroup(node.kind)) {
        return {};
    }
    if (IsChoiceOption(node.kind)) {
        return node.parent ? std::string_view(node.parent->action) : std::string_view{};
    }
    return node.action;
}

std::uint64_t Scene::ControlRevision(const Node &node) const
{
    return IsChoiceOption(node.kind) && node.parent ? node.parent->control_revision
                                                    : node.control_revision;
}

bool Scene::Selected(const Node &node) const
{
    if (IsChoiceOption(node.kind)) {
        return node.parent && !node.option_key.empty() &&
               node.parent->selected_key == node.option_key;
    }
    return node.kind == Kind::Checkbox && node.checked;
}

bool Scene::ValidChoiceAssignment(const Node &node, DslProperty property,
                                  const PropertyValue &value) const
{
    if (property == DslProperty::PopupFor || property == DslProperty::ScrollPart ||
        (node.kind == Kind::ScrollView &&
         (property == DslProperty::Clip || property == DslProperty::Overflow ||
          property == DslProperty::Padding || property == DslProperty::PaddingX ||
          property == DslProperty::PaddingY || property == DslProperty::Action))) {
        return false;
    }
    if (!ValidSliderAssignment(node, property, value)) {
        return false;
    }
    if (property == DslProperty::OptionKey ||
        (IsChoiceOption(node.kind) && property == DslProperty::Action)) {
        return false; // Stable identity is fixed at construction, never rebound.
    }
    if (property != DslProperty::SelectedKey) {
        return true;
    }
    const auto *key = std::get_if<std::string>(&value);
    if (!IsChoiceGroup(node.kind) || !key) {
        return false;
    }
    if (key->empty()) {
        return true; // Uninitialized/cleared group waits for authoritative state.
    }
    return std::any_of(node.children.begin(), node.children.end(),
                       [key](const auto &child) { return child->option_key == *key; });
}

void Scene::ValidateChoiceTree(const Node &node) const
{
    if (IsChoiceGroup(node.kind)) {
        if (node.children.empty() || node.children.size() > 128 || !node.region.empty()) {
            throw std::invalid_argument(
                "Choice group requires 1..128 direct options, without a region");
        }
        std::set<std::string> keys;
        const auto expected = node.kind == Kind::RadioGroup ? Kind::Radio : Kind::Segment;
        for (const auto &child : node.children) {
            if (child->kind != expected || !keys.insert(child->option_key).second) {
                throw std::invalid_argument("Choice group has mixed or duplicate options");
            }
        }
        if (!ValidChoiceAssignment(node, DslProperty::SelectedKey, node.selected_key)) {
            throw std::invalid_argument("Unknown selected choice key");
        }
        for (const Node *parent = node.parent; parent; parent = parent->parent) {
            if (parent->kind == Kind::Visual || IsChoiceOption(parent->kind)) {
                throw std::invalid_argument("Choice group cannot be nested in an option or Visual");
            }
        }
    }
    if (IsChoiceOption(node.kind)) {
        const auto expected = node.kind == Kind::Radio ? Kind::RadioGroup : Kind::SegmentGroup;
        if (!node.parent || node.parent->kind != expected || !node.region.empty() ||
            !scene_detail::ValidPropertyValue(DslProperty::OptionKey, node.option_key) ||
            !node.action.empty()) {
            throw std::invalid_argument("Choice option requires a group and a stable literal key");
        }
        for (const auto &binding : node.bindings) {
            if (binding.target == DslProperty::OptionKey || binding.target == DslProperty::Action) {
                throw std::invalid_argument("Choice identity/action cannot be bound on an option");
            }
        }
        for (const auto &ref : node.theme_refs) {
            if (ref.target == DslProperty::OptionKey || ref.target == DslProperty::Action) {
                throw std::invalid_argument("Choice identity/action cannot come from a theme");
            }
        }
    }
    for (const auto &child : node.children) {
        ValidateChoiceTree(*child);
    }
}

std::vector<Scene::Node *> Scene::ChoiceOptions(const Node &group, const InputSnapshot *snapshot,
                                                bool submitted) const
{
    std::vector<Node *> options;
    for (const auto &child : group.children) {
        if (IsInteractive(child->id) && (!submitted || IsInteractive(child->id, snapshot))) {
            options.push_back(child.get());
        }
    }
    return options;
}

Scene::Node *Scene::ChoiceTabStop(const Node &group, const InputSnapshot *snapshot,
                                  bool submitted) const
{
    const auto options = ChoiceOptions(group, snapshot, submitted);
    for (auto *option : options) {
        if (option->option_key == group.selected_key) {
            return option;
        }
    }
    return options.empty() ? nullptr : options.front();
}

std::optional<Activation> Scene::HandleChoiceKey(const contracts::KeyEvent &event,
                                                 const InputSnapshot *snapshot, bool submitted)
{
    const auto focus = std::find_if(
        input_state_->focus.begin(), input_state_->focus.end(),
        [&event](const InputState::Focus &item) { return item.seat == event.source.seat; });
    auto *current = focus == input_state_->focus.end() ? nullptr : Find(focus->node);
    if (!current || !IsChoiceOption(current->kind) || !IsInteractive(current->id)) {
        return std::nullopt;
    }
    auto options = ChoiceOptions(*current->parent, snapshot, submitted);
    const auto found = std::find(options.begin(), options.end(), current);
    if (found == options.end()) {
        return std::nullopt;
    }

    auto index = static_cast<std::size_t>(found - options.begin());
    if (event.physical_key == 0x4a) { // Home
        index = 0;
    } else if (event.physical_key == 0x4d) { // End
        index = options.size() - 1;
    } else if (event.physical_key == 0x50 || event.physical_key == 0x52) { // Left / Up
        index = (index + options.size() - 1) % options.size();
    } else {
        index = (index + 1) % options.size();
    }
    auto *next = options[index];
    CancelControlCapture(next->id);
    SetInputFocus(next->id, event.source.seat, true);
    return Activation{next->id, std::string(InputAction(*next))};
}
} // namespace prism::runtime
