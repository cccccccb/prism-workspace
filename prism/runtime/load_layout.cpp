#include "load_plan_p.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace prism::runtime {
struct PreparedLayout::Data {
    std::optional<PreparedComponent> body;
    std::vector<PreparedSlot> slots;
};

namespace {
struct LayoutRetentions {
    std::shared_ptr<const void> previous;
    std::shared_ptr<const void> next;
};

class LayoutCompiler {
public:
    LayoutCompiler(const LoadPlan &plan, const ComponentSource &source)
        : plan_(plan), source_(source)
    {
    }

    void Convert(SyntaxNode &node, std::vector<std::size_t> &path, bool placeholder = false,
                 bool visual = false)
    {
        visual = visual || node.name == "Visual";
        if (node.name == "Slot") {
            if (visual) {
                LoadSemanticError(source_, node.line, "Visual subtree cannot contain Slot");
            }
            if (placeholder) {
                LoadSemanticError(source_, node.line,
                                  "a Slot placeholder cannot contain another Slot");
            }
            ConvertSlot(node, path);
            placeholder = true;
        } else if (placeholder) {
            CheckInert(node);
        }
        std::size_t visual_index = 0;
        for (auto &child : node.children) {
            // Geometry declarations do not survive as prepared visual children.
            // Slot paths address that prepared tree, not raw syntax positions.
            if (child.name == "Contour") {
                continue;
            }
            path.push_back(visual_index++);
            Convert(child, path, placeholder, visual);
            path.pop_back();
        }
    }

    std::vector<PreparedSlot> Finish()
    {
        for (const auto &unit : plan_.components) {
            if (!seen_.contains(unit.id)) {
                LoadSemanticError(source_, 1, "component has no Slot: " + unit.id);
            }
        }
        return std::move(slots_);
    }

private:
    void ConvertSlot(SyntaxNode &node, const std::vector<std::size_t> &path)
    {
        const SyntaxArgument *component = nullptr;
        for (const auto &argument : node.arguments) {
            if (argument.name == "component") {
                if (component) {
                    LoadSemanticError(source_, argument.line, "duplicate Slot component field");
                }
                component = &argument;
            } else if (argument.name.empty()) {
                LoadSemanticError(source_, argument.line,
                                  "Slot does not accept positional arguments");
            }
        }
        const auto *id = component ? std::get_if<std::string>(&component->value.data) : nullptr;
        if (!id || !FindLoadUnit(plan_, *id)) {
            LoadSemanticError(source_, node.line, "Slot requires one known static component ID");
        }
        if (!seen_.insert(*id).second) {
            LoadSemanticError(source_, node.line,
                              "component appears in more than one Slot: " + *id);
        }
        slots_.push_back({*id, path, node.line});
        node.name = "Card";
        std::erase_if(node.arguments,
                      [](const SyntaxArgument &argument) { return argument.name == "component"; });
    }

    void CheckInert(const SyntaxNode &node) const
    {
        if (node.name == "Button" || node.name == "IconButton" || node.name == "Toggle" ||
            node.name == "InteractionTarget") {
            LoadSemanticError(source_, node.line,
                              "Slot placeholder cannot contain interactive controls");
        }
        for (const auto &argument : node.arguments) {
            if (argument.name == "action") {
                LoadSemanticError(source_, argument.line,
                                  "Slot placeholder cannot contain actions");
            }
        }
        for (const auto &modifier : node.modifiers) {
            if (modifier.name == "action") {
                LoadSemanticError(source_, modifier.line,
                                  "Slot placeholder cannot contain actions");
            }
        }
    }

    const LoadPlan &plan_;
    const ComponentSource &source_;
    std::unordered_set<std::string> seen_;
    std::vector<PreparedSlot> slots_;
};
} // namespace

PreparedLayout::PreparedLayout(std::shared_ptr<const Data> data) : data_(std::move(data))
{
}

PreparedLayout PreparedLayout::Legacy()
{
    return PreparedLayout(std::make_shared<Data>());
}

bool PreparedLayout::IsLegacy() const
{
    return !data_ || !data_->body;
}

const PreparedComponent &PreparedLayout::Body() const
{
    if (IsLegacy()) {
        throw std::logic_error("Legacy layout has no standalone body");
    }
    return *data_->body;
}

std::span<const PreparedSlot> PreparedLayout::Slots() const
{
    return data_ ? std::span<const PreparedSlot>(data_->slots) : std::span<const PreparedSlot>{};
}

std::size_t PreparedLayout::RetainedBytes() const
{
    if (!data_) {
        return 0;
    }
    std::size_t bytes = sizeof(Data) + data_->slots.capacity() * sizeof(PreparedSlot);
    if (data_->body) {
        bytes += data_->body->RetainedBytes();
    }
    for (const auto &slot : data_->slots) {
        bytes += slot.component.capacity() + slot.node_path.capacity() * sizeof(std::size_t);
    }
    return bytes;
}

PreparedLayout PreparedLayout::WithRetention(std::shared_ptr<const void> retention) const
{
    auto result = *this;
    if (retention_ && retention) {
        result.retention_ =
            std::make_shared<LayoutRetentions>(LayoutRetentions{retention_, std::move(retention)});
    } else if (retention) {
        result.retention_ = std::move(retention);
    }
    return result;
}

PreparedLayout PrepareLayout(std::string_view text, const LoadPlan &plan, ComponentSource metadata)
{
    if (plan.legacy) {
        return PreparedLayout::Legacy();
    }
    if (metadata.component_id.empty()) {
        metadata.component_id = "layout";
    }
    if (metadata.source_path.empty()) {
        metadata.source_path = (plan.package_root / plan.layout_path).string();
    }
    auto syntax = ReadLoadSyntax(text, metadata);
    LayoutCompiler compiler(plan, metadata);
    std::vector<std::size_t> path;
    compiler.Convert(syntax, path);
    auto data = std::make_shared<PreparedLayout::Data>();
    data->slots = compiler.Finish();
    data->body = PrepareVisualSyntax(syntax, metadata, text.size());
    ValidateUnitBindings(plan, data->body->Root(), metadata);
    return PreparedLayout(std::move(data));
}

} // namespace prism::runtime
