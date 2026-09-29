#include "opacity_layers_p.hpp"
#include "render_geometry_p.hpp"
#include <algorithm>
#include <variant>

namespace prism::render_skia::detail {
namespace {
struct Layer {
    SkMatrix matrix;
    SkRect device_ink{SkRect::MakeEmpty()};
    bool known{true};
};

bool NeedsLayer(const contracts::DrawCommand &command)
{
    const auto *opacity = std::get_if<contracts::PushOpacity>(&command);
    return opacity && opacity->opacity > 0 && opacity->opacity < 1;
}

bool IsScope(const contracts::DrawCommand &command)
{
    return std::holds_alternative<contracts::PushClipRect>(command) ||
           std::holds_alternative<contracts::PushClipRoundedRect>(command) ||
           std::holds_alternative<contracts::PopClip>(command);
}
} // namespace

std::vector<std::optional<SkRect>> OpacityLayerHints(const contracts::DisplayList &list,
                                                     const ResourceTable &resources)
{
    if (std::none_of(list.commands.begin(), list.commands.end(), NeedsLayer)) {
        return {};
    }

    std::vector<Layer> layers;
    std::vector<std::size_t> active;
    std::vector<SkMatrix> transforms;
    auto matrix = SkMatrix::I();

    for (const auto &command : list.commands) {
        if (const auto *transform = std::get_if<contracts::PushTransform>(&command)) {
            transforms.push_back(matrix);
            matrix = SkMatrix::Concat(matrix, TransformMatrix(*transform));
        } else if (std::holds_alternative<contracts::PopTransform>(command)) {
            matrix = transforms.back();
            transforms.pop_back();
        } else if (std::holds_alternative<contracts::PushOpacity>(command)) {
            active.push_back(layers.size());
            layers.push_back({matrix});
        } else if (std::holds_alternative<contracts::PopOpacity>(command)) {
            const auto index = active.back();
            active.pop_back();
            if (!active.empty()) {
                auto &parent = layers[active.back()];
                parent.known = parent.known && layers[index].known;
                parent.device_ink.join(layers[index].device_ink);
            }
        } else if (!active.empty() && !IsScope(command)) {
            auto &layer = layers[active.back()];
            SkRect ink;
            if (!DeviceInkBounds(command, resources, matrix, &ink)) {
                layer.known = false;
                continue;
            }
            if (ink.isEmpty()) {
                continue;
            }
            ink.outset(2, 2);
            layer.device_ink.join(ink);
        }
    }

    std::vector<std::optional<SkRect>> result;
    result.reserve(layers.size());
    for (const auto &layer : layers) {
        SkMatrix inverse;
        if (!layer.known || !layer.matrix.invert(&inverse)) {
            result.emplace_back();
            continue;
        }
        const auto local = inverse.mapRect(layer.device_ink);
        result.emplace_back(local.isFinite() ? std::optional<SkRect>(local) : std::nullopt);
    }
    return result;
}
} // namespace prism::render_skia::detail
