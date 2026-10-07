#include "scene_p.hpp"

namespace prism::runtime {
ValueCancelReason Scene::ControlEditInvalidation(const ControlEdit &edit) const
{
    const auto *node = Find(edit.node);
    if (!node || !IsControlTarget(node->kind) || !IsInteractive(node->id) ||
        InputAction(*node) != edit.action) {
        return ValueCancelReason::Unavailable;
    }
    if (ControlRevision(*node) != edit.event.revision) {
        return ValueCancelReason::Superseded;
    }
    if (IsChoiceOption(node->kind)) {
        const auto *before = std::get_if<std::string>(&edit.event.before);
        const auto *value = std::get_if<std::string>(&edit.event.value);
        return before && value && node->parent->selected_key == *before &&
                       node->option_key == *value
                   ? ValueCancelReason::None
                   : ValueCancelReason::Superseded;
    }
    if (node->kind == Kind::Slider) {
        const auto *before = std::get_if<double>(&edit.event.before);
        return before && node->value == *before ? ValueCancelReason::None
                                                : ValueCancelReason::Superseded;
    }
    const auto *before = std::get_if<bool>(&edit.event.before);
    return before && node->checked == *before ? ValueCancelReason::None
                                              : ValueCancelReason::Superseded;
}

bool Scene::IsCurrentControlEdit(const ControlEdit &edit) const
{
    return ControlEditInvalidation(edit) == ValueCancelReason::None;
}

void Scene::CancelControlCapture(contracts::NodeId id) noexcept
{
    const auto *node = Find(id);
    if (!node || !IsControlTarget(node->kind)) {
        return;
    }
    const auto *owner = IsChoiceOption(node->kind) ? node->parent : node;
    for (auto &pointer : input_state_->pointers) {
        const auto *captured = Find(pointer.captured);
        if (captured &&
            (captured == owner || (IsChoiceOption(captured->kind) && captured->parent == owner))) {
            pointer.captured = {};
            pointer.action.clear();
        }
    }
    std::erase_if(input_state_->keys, [this, owner](const InputState::KeyPress &key) {
        const auto *captured = Find(key.node);
        return captured &&
               (captured == owner || (IsChoiceOption(captured->kind) && captured->parent == owner));
    });
}

std::optional<ControlEdit> Scene::CommitControl(const Activation &activation)
{
    auto *node = Find(activation.node);
    if (!node || !IsControlTarget(node->kind) || !IsInteractive(node->id)) {
        return std::nullopt;
    }

    if (node->kind == Kind::Slider) {
        return std::nullopt; // Continuous input is handled by the Slider stream.
    }
    auto *owner = IsChoiceOption(node->kind) ? node->parent : node;
    ControlValue before = node->checked;
    ControlValue proposed = !node->checked;
    if (IsChoiceOption(node->kind)) {
        if (owner->selected_key == node->option_key) {
            return std::nullopt;
        }
        before = owner->selected_key;
        proposed = node->option_key;
        if (!owner->control_session) {
            ChoiceDomain domain;
            for (const auto &option : owner->children) {
                domain.keys.push_back(option->option_key);
            }
            owner->control_session = std::make_unique<ControlValueSession>(
                std::move(domain), before, owner->control_revision);
        }
    } else if (!owner->control_session) {
        owner->control_session =
            std::make_unique<ControlValueSession>(BooleanDomain{}, before, owner->control_revision);
    }

    owner->control_session->Synchronize(before, owner->control_revision);
    const auto interaction = owner->control_session->Begin();
    auto event = owner->control_session->Commit(interaction, std::move(proposed));
    return ControlEdit{node->id, std::string(InputAction(*node)), std::move(*event)};
}
} // namespace prism::runtime
