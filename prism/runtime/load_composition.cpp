#include "load_plan_p.hpp"
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace prism::runtime {
namespace {
struct CompositionRetention {
    PreparedLayout layout;
    std::vector<PreparedComponent> components;
};

class CriticalComposer {
public:
    CriticalComposer(const LoadPlan &plan, const PreparedLayout &layout,
                     std::span<const PreparedUnit> units)
        : plan_(plan), layout_(layout)
    {
        for (const auto &unit : units) {
            ValidatePreparedUnit(plan, unit.id, unit.prepared);
            if (!units_.emplace(unit.id, &unit.prepared).second) {
                LoadSemanticError(plan.source, 0, "duplicate prepared component: " + unit.id);
            }
        }
        for (const auto &unit : plan.components) {
            if (unit.phase == LoadPhase::Critical && !units_.contains(unit.id)) {
                LoadSemanticError(plan.source, unit.line, "missing critical component: " + unit.id);
            }
        }
    }

    PreparedComponent Compose()
    {
        if (plan_.legacy) {
            if (!layout_.IsLegacy() || plan_.components.size() != 1 || units_.size() != 1) {
                LoadSemanticError(plan_.source, 0,
                                  "legacy plan requires exactly one prepared Master");
            }
            return *units_.at(plan_.components.front().id);
        }
        if (layout_.IsLegacy()) {
            LoadSemanticError(plan_.source, 0, "Interface requires a prepared layout");
        }
        CheckSlots();
        auto retention = std::make_shared<CompositionRetention>(CompositionRetention{layout_, {}});
        source_bytes_ = plan_.source_bytes;
        aggregate_bytes_ = source_bytes_;
        AddSource(layout_.Body().SourceBytes(), layout_.Body().Source(), true);
        for (const auto &unit : plan_.components) {
            if (const auto found = units_.find(unit.id); found != units_.end()) {
                AddSource(found->second->SourceBytes(), found->second->Source(),
                          unit.phase == LoadPhase::Critical);
                if (unit.phase == LoadPhase::Critical) {
                    retention->components.push_back(*found->second);
                }
            }
        }
        std::vector<std::size_t> path;
        auto root = Copy(layout_.Body().Root(), layout_.Body(), path, 0, true);
        auto result = PreparedComponentAccess::Make(plan_.source, std::move(root),
                                                    std::move(images_), source_bytes_, node_count_);
        return result.WithRetention(std::move(retention));
    }

private:
    void CheckSlots() const
    {
        if (layout_.Slots().size() != plan_.components.size()) {
            LoadSemanticError(layout_.Body().Source(), 0, "layout Slot map does not match plan");
        }
        for (const auto &slot : layout_.Slots()) {
            if (!FindLoadUnit(plan_, slot.component)) {
                LoadSemanticError(layout_.Body().Source(), slot.line,
                                  "layout contains unknown Slot");
            }
        }
        ValidateUnitBindings(plan_, layout_.Body().Root(), layout_.Body().Source());
    }

    void AddSource(std::size_t bytes, const ComponentSource &source, bool composed)
    {
        if (bytes > kMaxLoadFileBytes || aggregate_bytes_ > kMaxLoadSourceBytes ||
            bytes > kMaxLoadSourceBytes - aggregate_bytes_) {
            LoadSemanticError(source, 0, "aggregate UI source exceeds 8 MiB limit");
        }
        aggregate_bytes_ += bytes;
        if (composed) {
            source_bytes_ += bytes;
        }
    }

    ImageReference Image(const PreparedComponent &owner, ImageReference reference, int line)
    {
        const auto table = owner.Images();
        if (reference.resource_key >= table.size()) {
            LoadSemanticError(owner.Source(), line, "prepared image reference is out of bounds");
        }
        const auto &uri = table[reference.resource_key].uri;
        if (const auto found = image_keys_.find(uri); found != image_keys_.end()) {
            return {found->second};
        }
        const auto key = images_.size();
        images_.push_back({uri, line});
        image_keys_.emplace(uri, key);
        return {key};
    }

    void Count(const PreparedNode &node, const ComponentSource &source, unsigned depth)
    {
        if (++node_count_ > 8192 || depth > 64) {
            LoadSemanticError(source, node.line, "composed UI exceeds 8192 nodes or depth 64");
        }
        bool effect = false;
        for (const auto &property : node.properties) {
            if (property.id == DslProperty::BackdropBlur && std::get<double>(property.value) > 0) {
                effect = true;
            }
        }
        for (const auto &binding : node.bindings) {
            effect |= binding.target == DslProperty::BackdropBlur;
        }
        for (const auto &reference : node.theme_refs) {
            effect |= reference.target == DslProperty::BackdropBlur;
        }
        if (effect && ++effect_count_ > 8) {
            LoadSemanticError(source, node.line, "composed UI surface effect region limit is 8");
        }
    }

    PreparedNode Copy(const PreparedNode &node, const PreparedComponent &owner,
                      std::vector<std::size_t> &path, unsigned depth, bool is_layout)
    {
        Count(node, owner.Source(), depth);
        PreparedNode out;
        out.kind = node.kind;
        out.bindings = node.bindings;
        out.theme_refs = node.theme_refs;
        out.transitions = node.transitions;
        out.state_rules = node.state_rules;
        out.contour = node.contour;
        out.contour_recipe = node.contour_recipe;
        out.allowed_properties = node.allowed_properties;
        out.line = node.line;
        for (const auto &property : node.properties) {
            auto value = property.value;
            if (const auto *image = std::get_if<ImageReference>(&value)) {
                value = Image(owner, *image, node.line);
            }
            out.properties.push_back({property.id, std::move(value)});
        }

        const PreparedSlot *slot = nullptr;
        if (is_layout) {
            for (const auto &candidate : layout_.Slots()) {
                if (candidate.node_path == path) {
                    slot = &candidate;
                    break;
                }
            }
        }
        if (slot) {
            out.region = slot->component;
            out.region_mounted = FindLoadUnit(plan_, slot->component)->phase == LoadPhase::Critical;
        }
        if (slot && FindLoadUnit(plan_, slot->component)->phase == LoadPhase::Critical) {
            const auto &component = *units_.at(slot->component);
            std::vector<std::size_t> unused;
            out.children.push_back(Copy(component.Root(), component, unused, depth + 1, false));
        } else {
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                path.push_back(i);
                out.children.push_back(Copy(node.children[i], owner, path, depth + 1, is_layout));
                path.pop_back();
            }
        }
        return out;
    }

    const LoadPlan &plan_;
    const PreparedLayout &layout_;
    std::unordered_map<std::string, const PreparedComponent *> units_;
    std::vector<PreparedImage> images_;
    std::unordered_map<std::string, std::size_t> image_keys_;
    std::size_t source_bytes_{}, aggregate_bytes_{}, node_count_{}, effect_count_{};
};
} // namespace

PreparedComponent ComposeCritical(const LoadPlan &plan, const PreparedLayout &layout,
                                  std::span<const PreparedUnit> units)
{
    return CriticalComposer(plan, layout, units).Compose();
}

} // namespace prism::runtime
