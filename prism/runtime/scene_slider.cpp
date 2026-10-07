#include "prism/runtime/scene_snapshot.hpp"
#include "scene_p.hpp"

#include <stdexcept>

namespace prism::runtime {
namespace {
bool DomainProperty(DslProperty property)
{
    return property == DslProperty::Minimum || property == DslProperty::Maximum ||
           property == DslProperty::Step || property == DslProperty::SliderPart;
}
} // namespace

bool Scene::ValidSliderAssignment(const Node &node, DslProperty property,
                                  const PropertyValue &value) const
{
    if (DomainProperty(property)) {
        return false; // Domain and part identity are fixed when the component is prepared.
    }
    if (property != DslProperty::Value) {
        return true;
    }
    const auto *number = std::get_if<double>(&value);
    if (!number || !std::isfinite(*number)) {
        return false;
    }
    if (node.kind == Kind::Progress) {
        return *number >= 0 && *number <= 1;
    }
    return node.kind != Kind::Slider ||
           (*number >= node.number_domain.minimum && *number <= node.number_domain.maximum);
}

void Scene::ValidateSliderTree(Node &node)
{
    for (const auto &binding : node.bindings) {
        if (DomainProperty(binding.target)) {
            throw std::invalid_argument("Slider domain/part cannot be bound");
        }
    }
    for (const auto &ref : node.theme_refs) {
        if (DomainProperty(ref.target)) {
            throw std::invalid_argument("Slider domain/part cannot come from a theme");
        }
    }
    if (!node.slider_part.empty() &&
        (node.kind != Kind::Visual || !node.parent || node.parent->kind != Kind::Slider ||
         !node.children.empty())) {
        throw std::invalid_argument("Slider part must be a direct leaf Visual of Slider");
    }
    if (node.kind == Kind::Slider) {
        const auto &domain = node.number_domain;
        if (domain.maximum <= domain.minimum || domain.step < 0) {
            throw std::invalid_argument("Slider requires minimum < maximum and step >= 0");
        }
        if (!node.properties.contains(DslProperty::Value)) {
            node.value = domain.minimum; // A bound value may arrive after construction.
        }
        if (!ValidSliderAssignment(node, DslProperty::Value, node.value)) {
            throw std::invalid_argument("Slider value outside its domain");
        }
        std::set<std::string> parts;
        for (const auto &child : node.children) {
            if (child->kind != Kind::Visual ||
                (!child->slider_part.empty() && !parts.insert(child->slider_part).second)) {
                throw std::invalid_argument("Slider requires unique decorative parts");
            }
            if (!child->slider_part.empty() &&
                (child->style.height <= 0 ||
                 (child->slider_part == "thumb" && child->style.width <= 0))) {
                throw std::invalid_argument("Slider parts need explicit height and thumb width");
            }
        }
        if (parts != std::set<std::string>{"track", "fill", "thumb"}) {
            throw std::invalid_argument("Slider requires track, fill and thumb Visuals");
        }
    }
    if (node.kind == Kind::Progress && (node.value < 0 || node.value > 1)) {
        throw std::invalid_argument("Progress value must be within 0..1");
    }
    for (auto &child : node.children) {
        ValidateSliderTree(*child);
    }
}

contracts::LogicalRect Scene::SliderTrack(const Node &node) const
{
    const double px = node.style.padding_x < 0 ? node.style.padding : node.style.padding_x;
    const double py = node.style.padding_y < 0 ? node.style.padding : node.style.padding_y;
    const double width = std::max(0.0, node.bounds.width - 2 * px);
    const double height = std::max(0.0, node.bounds.height - 2 * py);
    double thumb_width = 0;
    for (const auto &child : node.children) {
        if (child->slider_part == "thumb") {
            thumb_width = std::min(width, child->style.width);
        }
    }
    return {node.bounds.x + px + thumb_width / 2, node.bounds.y + py + height / 2,
            std::max(0.0, width - thumb_width), height};
}

double Scene::SliderPresentedValue(const Node &node) const
{
    for (const auto &stream : input_state_->sliders) {
        if (stream.node == node.id && !stream.terminal) {
            return std::get<double>(stream.session.PresentedValue());
        }
    }
    return node.value;
}

void Scene::ApplySliderVisuals(SceneSnapshot &snapshot) const
{
    for (const auto *node : nodes_) {
        if (!node || node->kind != Kind::Slider || !IsVisible(*node)) {
            continue;
        }
        const auto track = SliderTrack(*node);
        const auto &domain = node->number_domain;
        const double fraction = std::clamp((SliderPresentedValue(*node) - domain.minimum) /
                                               (domain.maximum - domain.minimum),
                                           0.0, 1.0);
        for (const auto &child : node->children) {
            if (child->slider_part.empty()) {
                continue;
            }
            auto &part = snapshot.Get(child->id);
            const double height = std::min(track.height, child->style.height);
            const double px =
                node->style.padding_x < 0 ? node->style.padding : node->style.padding_x;
            const double width =
                std::min(std::max(0.0, node->bounds.width - 2 * px), child->style.width);
            if (child->slider_part == "thumb") {
                part.bounds = {track.x + track.width * fraction - width / 2, track.y - height / 2,
                               width, height};
            } else {
                part.bounds = {track.x, track.y - height / 2,
                               child->slider_part == "fill" ? track.width * fraction : track.width,
                               height};
            }
        }
    }
}
} // namespace prism::runtime
