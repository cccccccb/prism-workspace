#include "prism/contracts/rounded_region.hpp"
#include "prism/runtime/popup_surface.hpp"
#include "prism/runtime/scene.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

using namespace prism;
using namespace prism::runtime;

namespace {
struct ShapeCounter {
    std::size_t *calls{};

    ShapedText operator()(std::string_view text, double font) const
    {
        ++*calls;
        return {{}, text.size() * font / 2, font};
    }
};

Blueprint Box(double width = 0, double height = 0)
{
    Blueprint node;
    node.properties = {{DslProperty::Width, width}, {DslProperty::Height, height}};
    return node;
}

Blueprint Layout(bool attached = true, bool rounded_child = false)
{
    Blueprint root;
    auto column = Box();
    column.kind = Kind::Column;
    column.children.push_back(Box(0, 80));
    auto row = Box(0, 32);
    row.kind = Kind::Row;
    auto target = Box(64, 32);
    target.kind = Kind::InteractionTarget;
    target.properties.push_back({DslProperty::Action, std::string("open")});
    row.children = {Box(180, 32), std::move(target)};
    column.children.push_back(std::move(row));
    root.children.push_back(std::move(column));

    auto popup = Box(260, 180);
    popup.kind = Kind::Popup;
    popup.properties.push_back({DslProperty::PopupFor, std::string("open")});
    popup.properties.push_back({DslProperty::Padding, 12.0});
    popup.properties.push_back({DslProperty::Background, contracts::Color{28, 40, 62, 110}});
    popup.properties.push_back({DslProperty::ShadowBlur, 6.0});
    popup.properties.push_back({DslProperty::ShadowY, 4.0});
    popup.properties.push_back({DslProperty::ShadowColor, contracts::Color{0, 0, 0, 160}});
    if (attached) {
        popup.contour_recipe = AttachedPanelRecipe{12.0, 28.0, 16.0, ContourFallback::Detached};
    }
    if (!rounded_child) {
        popup.properties.push_back({DslProperty::BackdropBlur, 12.0});
    }
    auto content = Box();
    content.kind = Kind::Column;
    if (rounded_child) {
        auto effect = Box(200, 140);
        effect.properties.push_back({DslProperty::Radius, 12.0});
        effect.properties.push_back({DslProperty::BackdropBlur, 8.0});
        content.children.push_back(std::move(effect));
    } else {
        auto text = Box(0, 20);
        text.kind = Kind::Text;
        text.properties.push_back({DslProperty::Text, std::string("Volume")});
        content.children.push_back(std::move(text));
    }
    popup.children.push_back(std::move(content));
    root.children.push_back(std::move(popup));
    return root;
}

struct Fixture {
    std::size_t shape_calls{};
    Scene scene;

    explicit Fixture(Blueprint blueprint = Layout())
        : scene(std::move(blueprint), ShapeCounter{&shape_calls})
    {
        assert(scene.SetViewport({640, 480}));
        assert(scene.Build({17}));
        contracts::NodeId anchor;
        for (const auto &node : scene.InputGeometry()->nodes) {
            if (node.id && node.action == "open") {
                anchor = node.id;
            }
        }
        assert(anchor && scene.OpenPopup(anchor));
        assert(scene.Build({17}));
        scene.AcknowledgeComposite();
    }

    PopupSurfaceRequest Request()
    {
        const auto request = scene.CapturePopupSurfaceRequest(4);
        assert(request);
        return *request;
    }
};

PopupSurfaceConfigure Configure(const PopupSurfaceRequest &request, bool above = false,
                                double width = 200, double height = 156)
{
    return {4,
            9,
            {std::floor(request.anchor.x + request.anchor.width / 2 - width / 2),
             above ? std::floor(request.anchor.y - request.gap - height)
                   : std::ceil(request.anchor.y + request.anchor.height + request.gap),
             width, height}};
}

PopupSurfaceIdentity Identity(std::uint64_t sequence = 1)
{
    return {3, 51, 51, 9, sequence};
}

void ConfiguredEffectsShareContourAndInputGeometry()
{
    Fixture f;
    const auto request = f.Request();
    assert(request.requires_backdrop);
    const auto root_effects = f.scene.SurfaceEffects();
    const auto root_input = f.scene.InputGeometry();
    const auto root_bounds = f.scene.Bounds(request.active_node);
    const auto root_stats = f.scene.GetRenderStats();

    for (bool above : {false, true}) {
        const auto plan = f.scene.PreparePopupSurface(request, Configure(request, above));
        assert(plan && plan->effect_regions.size() == 1);
        const auto &effect = plan->effect_regions.front();
        contracts::ValidateSurfaceEffectRegion(effect);
        const auto *input = plan->input_snapshot->Find(request.active_node);
        assert(input && input->contour && effect.contour == *input->contour);
        assert(effect.bounds == plan->window_geometry);
        assert(effect.corner_radius == 0 && effect.blur_radius == 12);
        assert(effect.bounds.width == 200 && effect.bounds.height == 156);
        assert(plan->buffer_size.width > effect.bounds.width);
        assert(!contracts::ContourContains(*effect.contour, {1.5, 1.5}));
        const double neck_x = request.anchor.x + request.anchor.width / 2 - plan->surface_origin.x;
        const double neck_y = above ? plan->body_geometry.y + plan->body_geometry.height + 8
                                    : plan->window_geometry.y + 8;
        assert(contracts::ContourContains(*effect.contour, {neck_x, neck_y}));
    }
    assert(f.scene.InputGeometry() == root_input && f.scene.SurfaceEffects() == root_effects);
    assert(f.scene.Bounds(request.active_node) == root_bounds);
    assert(f.scene.GetRenderStats() == root_stats);
}

void EffectOnlyChangesReuseConfiguredLayout()
{
    Fixture f;
    const auto request = f.Request();
    const auto initial = f.scene.PreparePopupSurface(request, Configure(request));
    assert(initial);
    const auto stats = f.scene.GetPopupSurfacePreparationStats();
    const auto shapes = f.shape_calls;
    const auto root_stats = f.scene.GetRenderStats();
    const auto pixels = f.scene.PixelsRevision();

    assert(f.scene.SetProperty(request.active_node, DslProperty::BackdropBlur, 24.0));
    assert(!f.scene.Build({17}));
    const auto current = f.Request();
    const auto next = f.scene.PreparePopupSurface(current, Configure(current));
    assert(next && next->effect_regions.front().blur_radius == 24);
    assert(next->effect_regions.front().contour == initial->effect_regions.front().contour);
    assert(next->display_list->commands == initial->display_list->commands);
    assert(f.scene.PixelsRevision() == pixels);
    assert(f.scene.GetPopupSurfacePreparationStats().layouts == stats.layouts);
    assert(f.scene.GetPopupSurfacePreparationStats().layout_reuses == stats.layout_reuses + 1);
    assert(f.scene.GetRenderStats().layouts == root_stats.layouts && f.shape_calls == shapes);

    assert(f.scene.SetProperty(request.active_node, DslProperty::BackdropBlur, 0.0));
    assert(!f.scene.Build({17}));
    const auto clear_request = f.Request();
    const auto clear = f.scene.PreparePopupSurface(clear_request, Configure(clear_request));
    assert(clear && clear->effect_regions.empty() && !clear_request.requires_backdrop);
    assert(clear->display_list->commands == initial->display_list->commands);
}

void ProvenanceAndRootEffectHandoff()
{
    Fixture f;
    const auto request = f.Request();
    const auto plan = f.scene.PreparePopupSurface(request, Configure(request));
    assert(plan);
    const auto original = f.scene.SurfaceEffects();
    assert(original.size() == 1);

    auto forged = *plan;
    forged.effect_regions.clear();
    assert(!f.scene.AdoptPopupSurface(forged, Identity()));
    forged = *plan;
    forged.effect_regions.front().blur_radius += 1;
    assert(!f.scene.AdoptPopupSurface(forged, Identity()));
    forged = *plan;
    forged.effect_regions.front().contour->points.front().x += 1;
    assert(!f.scene.AdoptPopupSurface(forged, Identity()));

    assert(f.scene.AdoptPopupSurface(*plan, Identity()));
    assert(f.scene.Build({17}));
    assert(f.scene.SurfaceEffects().empty());
    const auto adopted_request = f.Request();
    const auto child = f.scene.PreparePopupSurface(adopted_request, Configure(adopted_request));
    assert(child && child->effect_regions == plan->effect_regions);
    assert(f.scene.RevokePopupSurface(Identity()));
    assert(f.scene.Build({17}));
    assert(f.scene.SurfaceEffects() == original);
}

void UnsupportedFinalClippingKeepsRootFallback()
{
    Fixture f(Layout(false, true));
    const auto request = f.Request();
    const auto original = f.scene.SurfaceEffects();
    const auto root_input = f.scene.InputGeometry();
    const auto stats = f.scene.GetRenderStats();
    std::string diagnostic;

    assert(original.size() == 1 && original.front().corner_radius == 12);
    const auto valid = f.scene.PreparePopupSurface(request, Configure(request, false, 240, 180));
    assert(valid && valid->effect_regions.size() == 1);
    const auto preparation_stats = f.scene.GetPopupSurfacePreparationStats();
    const auto cropped =
        f.scene.PreparePopupSurface(request, Configure(request, false, 180, 100), &diagnostic);
    assert(!cropped && diagnostic.find("Unsupported backdrop clipping") != std::string::npos);
    assert(f.scene.GetPopupSurfacePreparationStats() == preparation_stats);
    assert(f.scene.InputGeometry() == root_input && f.scene.SurfaceEffects() == original);
    assert(f.scene.GetRenderStats() == stats && f.scene.PopupToken() == request.popup_token);
    assert(!f.scene.HasPopupSurfaceAdoption());
}

void FinalEffectBudgetIsIndependentFromRootVisibility()
{
    auto blueprint = Layout(false);
    auto &popup = blueprint.children.back();
    for (auto &property : popup.properties) {
        if (property.id == DslProperty::Height) {
            property.value = 160.0;
        } else if (property.id == DslProperty::BackdropBlur) {
            property.value = 0.0;
        }
    }
    auto &content = popup.children.front();
    content.children.clear();
    for (int i = 0; i != 9; ++i) {
        auto effect = Box(0, 20);
        effect.properties.push_back({DslProperty::BackdropBlur, 2.0});
        content.children.push_back(std::move(effect));
    }
    Fixture f(std::move(blueprint));
    const auto request = f.Request();
    const auto original = f.scene.SurfaceEffects();
    assert(original.size() == 8);
    const auto fitted = f.scene.PreparePopupSurface(request, Configure(request, false, 240, 160));
    assert(fitted && fitted->effect_regions.size() == 8);
    const auto stats = f.scene.GetPopupSurfacePreparationStats();
    std::string diagnostic;

    // Native configure can expose rows clipped in the root fallback. Validate
    // this target's final visible regions rather than borrowing the root count.
    const auto expanded =
        f.scene.PreparePopupSurface(request, Configure(request, false, 240, 260), &diagnostic);
    assert(!expanded && diagnostic.find("effect region limit is 8") != std::string::npos);
    assert(f.scene.GetPopupSurfacePreparationStats() == stats);
    assert(f.scene.SurfaceEffects() == original && f.scene.PopupToken() == request.popup_token);
}
} // namespace

int main()
{
    ConfiguredEffectsShareContourAndInputGeometry();
    EffectOnlyChangesReuseConfiguredLayout();
    ProvenanceAndRootEffectHandoff();
    UnsupportedFinalClippingKeepsRootFallback();
    FinalEffectBudgetIsIndependentFromRootVisibility();
}
