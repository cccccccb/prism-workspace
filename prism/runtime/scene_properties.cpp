#include "scene_p.hpp"

namespace prism::runtime {
void Scene::ApplyCachedProperty(Node &node, DslProperty id, const PropertyValue &value)
{
    switch (id) {
    case DslProperty::TooltipFor:
        node.tooltip_for = std::get<std::string>(value);
        break;
    case DslProperty::TooltipDelayMs:
        node.tooltip_delay_ms = std::get<double>(value);
        break;
    case DslProperty::PopupFor:
        node.popup_for = std::get<std::string>(value);
        break;
    case DslProperty::ScrollSpeed:
        node.scroll_speed = std::get<double>(value);
        break;
    case DslProperty::ScrollPart:
        node.scroll_part = std::get<std::string>(value);
        break;
    case DslProperty::Minimum:
        node.number_domain.minimum = std::get<double>(value);
        break;
    case DslProperty::Maximum:
        node.number_domain.maximum = std::get<double>(value);
        break;
    case DslProperty::Step:
        node.number_domain.step = std::get<double>(value);
        break;
    case DslProperty::SliderPart:
        node.slider_part = std::get<std::string>(value);
        break;
    case DslProperty::SelectedKey:
        if (node.selected_key != std::get<std::string>(value)) {
            ++node.control_revision;
        }
        node.selected_key = std::get<std::string>(value);
        break;
    case DslProperty::OptionKey:
        node.option_key = std::get<std::string>(value);
        break;
    case DslProperty::TranslateX:
        node.presentation.translate_x = std::get<double>(value);
        break;
    case DslProperty::TranslateY:
        node.presentation.translate_y = std::get<double>(value);
        break;
    case DslProperty::ScaleX:
        node.presentation.scale_x = std::get<double>(value);
        break;
    case DslProperty::ScaleY:
        node.presentation.scale_y = std::get<double>(value);
        break;
    case DslProperty::OriginX:
        node.presentation.origin_x = std::get<double>(value);
        break;
    case DslProperty::OriginY:
        node.presentation.origin_y = std::get<double>(value);
        break;
    case DslProperty::Opacity:
        node.presentation.opacity = std::get<double>(value);
        break;
    case DslProperty::MinViewportWidth:
        node.style.min_viewport_width = std::get<double>(value);
        break;
    case DslProperty::MaxViewportWidth:
        node.style.max_viewport_width = std::get<double>(value);
        break;
    case DslProperty::MinViewportHeight:
        node.style.min_viewport_height = std::get<double>(value);
        break;
    case DslProperty::MaxViewportHeight:
        node.style.max_viewport_height = std::get<double>(value);
        break;
    case DslProperty::Enabled:
        node.enabled = std::get<bool>(value);
        break;
    case DslProperty::Visible:
        node.style.visible = std::get<bool>(value);
        break;
    case DslProperty::Width:
        node.style.width = std::get<double>(value);
        break;
    case DslProperty::Height:
        node.style.height = std::get<double>(value);
        break;
    case DslProperty::LineHeight:
        node.style.line_height = std::get<double>(value);
        break;
    case DslProperty::Font:
        node.style.font_size = std::get<double>(value);
        break;
    case DslProperty::Spacing:
        node.style.spacing = std::get<double>(value);
        break;
    case DslProperty::Padding:
        node.style.padding = std::get<double>(value);
        break;
    case DslProperty::Radius:
        node.style.radius = std::get<double>(value);
        break;
    case DslProperty::Background:
        node.style.background = std::get<contracts::Color>(value);
        break;
    case DslProperty::Foreground:
        node.style.foreground = std::get<contracts::Color>(value);
        break;
    case DslProperty::Clip:
        node.style.clip = std::get<bool>(value);
        break;
    case DslProperty::Text:
        if (node.kind == Kind::TextField || node.kind == Kind::TextArea) {
            node.editor.Assign(std::get<std::string>(value));
        }
        node.text = std::get<std::string>(value);
        break;
    case DslProperty::Action:
        node.action = std::get<std::string>(value);
        break;
    case DslProperty::Align:
        node.style.align = std::get<std::string>(value);
        break;
    case DslProperty::Justify:
        node.style.justify = std::get<std::string>(value);
        break;
    case DslProperty::Anchor:
        node.style.anchor = std::get<std::string>(value);
        break;
    case DslProperty::Overflow:
        node.style.overflow = std::get<std::string>(value);
        break;
    case DslProperty::Flex:
        node.style.flex = std::get<double>(value);
        break;
    case DslProperty::Inset:
        node.style.inset = std::get<double>(value);
        break;
    case DslProperty::PaddingX:
        node.style.padding_x = std::get<double>(value);
        break;
    case DslProperty::PaddingY:
        node.style.padding_y = std::get<double>(value);
        break;
    case DslProperty::BorderWidth:
        node.style.border_width = std::get<double>(value);
        break;
    case DslProperty::BorderColor:
        node.style.border_color = std::get<contracts::Color>(value);
        break;
    case DslProperty::ShadowBlur:
        node.style.shadow_blur = std::get<double>(value);
        break;
    case DslProperty::ShadowY:
        node.style.shadow_y = std::get<double>(value);
        break;
    case DslProperty::ShadowColor:
        node.style.shadow_color = std::get<contracts::Color>(value);
        break;
    case DslProperty::InnerShadowBlur:
        node.style.inner_shadow_blur = std::get<double>(value);
        break;
    case DslProperty::InnerShadowY:
        node.style.inner_shadow_y = std::get<double>(value);
        break;
    case DslProperty::InnerShadowColor:
        node.style.inner_shadow_color = std::get<contracts::Color>(value);
        break;
    case DslProperty::BackdropBlur:
        node.style.backdrop_blur = std::get<double>(value);
        break;
    case DslProperty::Material:
        node.style.material = std::get<std::string>(value);
        break;
    case DslProperty::InputShape:
        node.style.input_shape = std::get<std::string>(value);
        break;
    case DslProperty::Icon:
        node.icon = std::get<std::string>(value);
        break;
    case DslProperty::Value:
        if (node.kind == Kind::Slider && node.value != std::get<double>(value)) {
            ++node.control_revision;
        }
        node.value = std::get<double>(value);
        break;
    case DslProperty::Checked:
        if (node.checked != std::get<bool>(value)) {
            ++node.control_revision;
        }
        node.checked = std::get<bool>(value);
        break;
    case DslProperty::ImageFit: {
        const auto &name = std::get<std::string>(value);
        node.style.image_fit = name == "contain" ? contracts::ImageFit::Contain
                               : name == "cover" ? contracts::ImageFit::Cover
                                                 : contracts::ImageFit::Fill;
        break;
    }
    case DslProperty::Source:
        node.image = std::get<contracts::ResourceId>(value);
        node.image_ready = false;
        node.intrinsic_size = {};
        break;
    }
}

PropertyValue Scene::CurrentProperty(const Node &node, DslProperty id) const
{
    switch (id) {
    case DslProperty::TooltipFor:
        return node.tooltip_for;
    case DslProperty::TooltipDelayMs:
        return node.tooltip_delay_ms;
    case DslProperty::PopupFor:
        return node.popup_for;
    case DslProperty::ScrollSpeed:
        return node.scroll_speed;
    case DslProperty::ScrollPart:
        return node.scroll_part;
    case DslProperty::Minimum:
        return node.number_domain.minimum;
    case DslProperty::Maximum:
        return node.number_domain.maximum;
    case DslProperty::Step:
        return node.number_domain.step;
    case DslProperty::SliderPart:
        return node.slider_part;
    case DslProperty::SelectedKey:
        return node.selected_key;
    case DslProperty::OptionKey:
        return node.option_key;
    case DslProperty::TranslateX:
        return node.presentation.translate_x;
    case DslProperty::TranslateY:
        return node.presentation.translate_y;
    case DslProperty::ScaleX:
        return node.presentation.scale_x;
    case DslProperty::ScaleY:
        return node.presentation.scale_y;
    case DslProperty::OriginX:
        return node.presentation.origin_x;
    case DslProperty::OriginY:
        return node.presentation.origin_y;
    case DslProperty::Opacity:
        return node.presentation.opacity;
    case DslProperty::MinViewportWidth:
        return node.style.min_viewport_width;
    case DslProperty::MaxViewportWidth:
        return node.style.max_viewport_width;
    case DslProperty::MinViewportHeight:
        return node.style.min_viewport_height;
    case DslProperty::MaxViewportHeight:
        return node.style.max_viewport_height;
    case DslProperty::Enabled:
        return node.enabled;
    case DslProperty::Visible:
        return node.style.visible;
    case DslProperty::Width:
        return node.style.width;
    case DslProperty::Height:
        return node.style.height;
    case DslProperty::LineHeight:
        return node.style.line_height;
    case DslProperty::Font:
        return node.style.font_size;
    case DslProperty::Spacing:
        return node.style.spacing;
    case DslProperty::Padding:
        return node.style.padding;
    case DslProperty::Material:
        return node.style.material;
    case DslProperty::InputShape:
        return node.style.input_shape;
    case DslProperty::InnerShadowY:
        return node.style.inner_shadow_y;
    case DslProperty::Radius:
        return node.style.radius;
    case DslProperty::Background:
        return node.style.background;
    case DslProperty::Foreground:
        return node.style.foreground;
    case DslProperty::Clip:
        return node.style.clip;
    case DslProperty::Text:
        return node.text;
    case DslProperty::Action:
        return node.action;
    case DslProperty::Source:
        return node.image;
    case DslProperty::Align:
        return node.style.align;
    case DslProperty::Justify:
        return node.style.justify;
    case DslProperty::Anchor:
        return node.style.anchor;
    case DslProperty::Overflow:
        return node.style.overflow;
    case DslProperty::Flex:
        return node.style.flex;
    case DslProperty::Inset:
        return node.style.inset;
    case DslProperty::PaddingX:
        return node.style.padding_x;
    case DslProperty::PaddingY:
        return node.style.padding_y;
    case DslProperty::BorderWidth:
        return node.style.border_width;
    case DslProperty::BorderColor:
        return node.style.border_color;
    case DslProperty::ShadowBlur:
        return node.style.shadow_blur;
    case DslProperty::ShadowY:
        return node.style.shadow_y;
    case DslProperty::ShadowColor:
        return node.style.shadow_color;
    case DslProperty::InnerShadowBlur:
        return node.style.inner_shadow_blur;
    case DslProperty::InnerShadowColor:
        return node.style.inner_shadow_color;
    case DslProperty::BackdropBlur:
        return node.style.backdrop_blur;
    case DslProperty::Icon:
        return node.icon;
    case DslProperty::Value:
        return node.value;
    case DslProperty::Checked:
        return node.checked;
    case DslProperty::ImageFit:
        return std::string(node.style.image_fit == contracts::ImageFit::Cover     ? "cover"
                           : node.style.image_fit == contracts::ImageFit::Contain ? "contain"
                                                                                  : "fill");
    default: {
        const auto found = node.properties.find(id);
        if (found != node.properties.end()) {
            return found->second;
        }
        const auto *spec = FindProperty(id);
        if (spec->stored_type == StoredValueType::String) {
            return std::string{};
        }
        if (spec->stored_type == StoredValueType::Color) {
            return contracts::Color{0, 0, 0, 0};
        }
        if (spec->stored_type == StoredValueType::Boolean) {
            return false;
        }
        return 0.0;
    }
    }
    return {};
}

} // namespace prism::runtime
