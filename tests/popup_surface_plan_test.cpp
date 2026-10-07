#include "prism/contracts/contour.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include "prism/contracts/rounded_region.hpp"
#include "prism/runtime/popup_surface.hpp"
#include "prism/runtime/scene.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <string_view>
#include <variant>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::Color white{220, 225, 235, 255};

ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font / 2, font};
}

Blueprint Box(double width = 0, double height = 0)
{
    Blueprint result;
    result.properties = {{DslProperty::Width, width}, {DslProperty::Height, height}};
    return result;
}

Blueprint Target(std::string action, double width = 0, double height = 0)
{
    auto result = Box(width, height);
    result.kind = Kind::InteractionTarget;
    result.properties.push_back({DslProperty::Action, std::move(action)});
    return result;
}

Blueprint Slider()
{
    auto result = Box(0, 32);
    result.kind = Kind::Slider;
    result.properties.push_back({DslProperty::Action, std::string("volume")});
    result.properties.push_back({DslProperty::Value, 0.625});
    for (std::string role : {"track", "fill", "thumb"}) {
        auto part = Box(role == "thumb" ? 12 : 0, role == "thumb" ? 12 : 4);
        part.kind = Kind::Visual;
        part.properties.push_back({DslProperty::SliderPart, role});
        part.properties.push_back({DslProperty::Background, white});
        result.children.push_back(std::move(part));
    }
    return result;
}

Blueprint Layout(double lead = 80, bool editor = false, bool scroll = false,
                 double neck_height = 16, double left = 180)
{
    Blueprint root;
    Blueprint column;
    column.kind = Kind::Column;
    column.children.push_back(Box(0, lead));
    auto row = Box(0, 32);
    row.kind = Kind::Row;
    row.children = {Box(left, 32), Target("sound", 64, 32)};
    column.children.push_back(std::move(row));
    root.children.push_back(std::move(column));

    auto popup = Box(240, 180);
    popup.kind = Kind::Popup;
    popup.contour_recipe = AttachedPanelRecipe{12.0, 28.0, neck_height, ContourFallback::Detached};
    popup.properties.push_back({DslProperty::PopupFor, std::string("sound")});
    popup.properties.push_back({DslProperty::Padding, 12.0});
    popup.properties.push_back({DslProperty::Background, white});
    popup.properties.push_back({DslProperty::ShadowBlur, 6.0});
    popup.properties.push_back({DslProperty::ShadowY, 4.0});
    popup.properties.push_back({DslProperty::ShadowColor, contracts::Color{0, 0, 0, 180}});

    Blueprint content;
    content.kind = Kind::Column;
    content.properties.push_back({DslProperty::Spacing, 8.0});
    content.children.push_back(Slider());
    content.children.push_back(Target("apply", 0, 24));
    if (editor) {
        auto field = Box(0, 30);
        field.kind = Kind::TextField;
        field.properties.push_back({DslProperty::Action, std::string("edit")});
        field.properties.push_back({DslProperty::Text, std::string("Editor")});
        content.children.push_back(std::move(field));
    }
    if (scroll) {
        auto view = Box(0, 0);
        view.kind = Kind::ScrollView;
        auto flow = Box(0, 360);
        flow.kind = Kind::Column;
        flow.children.push_back(Target("scrolled", 0, 40));
        view.children.push_back(std::move(flow));
        auto thumb = Box(6, 14);
        thumb.kind = Kind::Visual;
        thumb.properties.push_back({DslProperty::ScrollPart, std::string("thumb")});
        thumb.properties.push_back({DslProperty::Background, white});
        view.children.push_back(std::move(thumb));
        content.children.push_back(std::move(view));
    }
    popup.children.push_back(std::move(content));
    root.children.push_back(std::move(popup));
    return root;
}

contracts::NodeId Action(const InputSnapshot &snapshot, std::string_view action)
{
    for (const auto &node : snapshot.nodes) {
        if (node.id && node.action == action) {
            return node.id;
        }
    }
    assert(false);
    return {};
}

struct Fixture {
    Scene scene;
    contracts::NodeId anchor;

    explicit Fixture(Blueprint layout = Layout(), contracts::LogicalSize size = {640, 480})
        : scene(std::move(layout), Shape)
    {
        assert(scene.SetViewport(size));
        assert(scene.Build({17}));
        anchor = Action(*scene.InputGeometry(), "sound");
        assert(scene.OpenPopup(anchor));
        assert(scene.Build({17}));
        scene.AcknowledgeComposite();
    }

    PopupSurfaceRequest Request(std::uint64_t generation = 4)
    {
        auto request = scene.CapturePopupSurfaceRequest(generation);
        assert(request);
        return *request;
    }
};

PopupSurfaceConfigure Configure(const PopupSurfaceRequest &request, bool above = false,
                                double width = 240, double height = 196)
{
    const double x = std::floor(request.anchor.x + request.anchor.width / 2 - width / 2);
    const double y = above ? std::floor(request.anchor.y - request.gap - height)
                           : std::ceil(request.anchor.y + request.anchor.height + request.gap);
    return {request.parent_configure_generation,
            9,
            {x - request.parent_window_geometry.x, y - request.parent_window_geometry.y, width,
             height}};
}

bool MaskContains(const PopupSurfacePlan &plan, contracts::LogicalPoint point)
{
    for (const auto &region : plan.input_regions) {
        if (contracts::RoundedRegionContains(point, region)) {
            return true;
        }
    }
    return false;
}

void RejectLocalInput(Scene &scene, const PopupSurfacePlan &plan)
{
    const auto id = Action(*plan.input_snapshot, "apply");
    const auto bounds = plan.input_snapshot->Find(id)->bounds;
    for (auto state : {contracts::ButtonState::Pressed, contracts::ButtonState::Released}) {
        const auto result =
            scene.HandleInput(contracts::PointerButtonEvent{{17},
                                                            {bounds.x + 1, bounds.y + 1},
                                                            contracts::PointerButton::Primary,
                                                            state,
                                                            0,
                                                            1,
                                                            {1, 1, 1}},
                              plan.input_snapshot);
        assert(!result.activation && !result.control_edit);
    }
}

void GeometryAndPurity()
{
    Fixture f;
    const auto request = f.Request();
    assert(request.desired_body == (contracts::LogicalSize{240, 180}));
    assert(request.desired_geometry == (contracts::LogicalSize{240, 196}));
    assert(f.Request().source == request.source); // No per-frame snapshot copy.
    const auto root_input = f.scene.InputGeometry();
    const auto root_regions = f.scene.InputRegions();
    const auto root_bounds = f.scene.Bounds(request.active_node);
    const auto root_stats = f.scene.GetRenderStats();
    const auto root_revision = f.scene.TransactionRevision();
    const auto root_pixels = f.scene.PixelsRevision();

    for (bool above : {false, true}) {
        const auto configure = Configure(request, above, 200, 156);
        const auto plan = f.scene.PreparePopupSurface(request, configure);
        assert(plan && plan->display_list && plan->input_snapshot);
        contracts::ValidateDisplayList(*plan->display_list);
        assert(plan->body_bounds.width == 200 && plan->body_bounds.height == 140);
        assert(plan->body_geometry.y == plan->window_geometry.y + (above ? 0 : 16));
        assert(plan->window_geometry.width == 200 && plan->window_geometry.height == 156);
        assert(plan->window_geometry.x >= 22 && plan->window_geometry.y >= 18);
        assert(plan->window_geometry.y + plan->window_geometry.height + 26 <=
               plan->buffer_size.height);
        const auto *panel = plan->input_snapshot->Find(request.active_node);
        assert(panel && panel->contour && panel->parent == contracts::NodeId{});
        const auto bounds = contracts::ContourBounds(*panel->contour);
        assert(bounds == plan->window_geometry);
        const double neck_x = request.anchor.x + request.anchor.width / 2 - plan->surface_origin.x;
        const double neck_y = above ? plan->body_geometry.y + plan->body_geometry.height + 8
                                    : plan->window_geometry.y + 8;
        assert(contracts::ContourContains(*panel->contour, {neck_x, neck_y}));
        assert(MaskContains(*plan, {std::floor(neck_x) + .5, std::floor(neck_y) + .5}));
        assert(!MaskContains(*plan, {1.5, 1.5}));
        assert(plan->input_snapshot->scene == 0 && request.scene != 0);
        assert(!f.scene.HitTest({neck_x, neck_y}, *plan->input_snapshot));
        assert(!f.scene.ApplyInputSnapshot(plan->input_snapshot));
        RejectLocalInput(f.scene, *plan);
        const auto *slider = plan->input_snapshot->Find(Action(*root_input, "volume"));
        assert(slider && slider->slider_track.width == 164);

        bool fill = false;
        for (const auto &command : plan->display_list->commands) {
            if (const auto *rect = std::get_if<contracts::FillRect>(&command);
                rect && rect->bounds.width == slider->slider_track.width * .625 &&
                rect->bounds.height == 4) {
                fill = true;
            }
        }
        assert(fill);
    }
    assert(f.scene.InputGeometry() == root_input);
    assert(f.scene.InputRegions() == root_regions);
    assert(f.scene.Bounds(request.active_node) == root_bounds);
    assert(f.scene.GetRenderStats() == root_stats);
    assert(f.scene.TransactionRevision() == root_revision &&
           f.scene.PixelsRevision() == root_pixels);
    assert(f.scene.PopupToken() == request.popup_token);
}

void MetadataAndLifetime()
{
    Fixture f;
    const auto request = f.Request();
    const auto configure = Configure(request);
    std::string diagnostic;
    auto forged = request;
    forged.anchor.x += 1;
    assert(!f.scene.PreparePopupSurface(forged, configure, &diagnostic) && !diagnostic.empty());
    forged = request;
    forged.desired_body.width += 1;
    assert(!f.scene.PreparePopupSurface(forged, configure));
    auto invalid = configure;
    invalid.window_bounds.x += .5;
    assert(!f.scene.PreparePopupSurface(request, invalid));
    invalid = configure;
    invalid.window_bounds.height = std::numeric_limits<double>::infinity();
    assert(!f.scene.PreparePopupSurface(request, invalid));
    invalid = configure;
    invalid.parent_configure_generation += 1;
    assert(!f.scene.PreparePopupSurface(request, invalid));
    invalid = configure;
    invalid.configure_generation = 0;
    assert(!f.scene.PreparePopupSurface(request, invalid));
    f.Request(5);
    assert(!f.scene.PreparePopupSurface(request, configure));
    const auto fresh = f.Request();
    assert(f.scene.ClosePopup());
    assert(!f.scene.PreparePopupSurface(fresh, Configure(fresh)));
    assert(!f.scene.CapturePopupSurfaceRequest(4));
    assert(f.scene.Build({17}));
    assert(f.scene.OpenPopup(f.anchor));
    assert(f.scene.Build({17}));
    assert(!f.scene.PreparePopupSurface(fresh, Configure(fresh)));
    const auto newer = f.Request();
    assert(newer.popup_token != fresh.popup_token);
    assert(f.scene.PreparePopupSurface(newer, Configure(newer)));
    assert(f.scene.SetProperty(f.anchor, DslProperty::Width, 65.0));
    assert(!f.scene.PreparePopupSurface(newer, Configure(newer)));
    assert(!f.scene.CapturePopupSurfaceRequest(4));
}

void ParentAndFallback()
{
    Fixture f;
    const auto request = f.scene.CapturePopupSurfaceRequest(12, {7, 11, 620, 450});
    assert(request);
    const auto configure = Configure(*request);
    const auto plan = f.scene.PreparePopupSurface(*request, configure);
    assert(plan && plan->body_bounds.x == configure.window_bounds.x + 7);
    assert(plan->body_bounds.y == configure.window_bounds.y + 11 + 16);

    auto slid = configure;
    slid.window_bounds.x = 410;
    const auto detached = f.scene.PreparePopupSurface(*request, slid);
    assert(detached && detached->body_geometry == detached->window_geometry);
    const auto *panel = detached->input_snapshot->Find(request->active_node);
    assert(panel && contracts::ContourBounds(*panel->contour) == detached->body_geometry);
    auto edge = configure;
    edge.window_bounds.x =
        request->anchor.x + request->anchor.width - 26 - request->parent_window_geometry.x;
    const auto boundary = f.scene.PreparePopupSurface(*request, edge);
    assert(boundary && boundary->body_geometry == boundary->window_geometry);
    auto tiny = configure;
    tiny.window_bounds.width = 12;
    tiny.window_bounds.height = 12;
    const auto compact = f.scene.PreparePopupSurface(*request, tiny);
    assert(compact && compact->body_geometry == compact->window_geometry);

    Fixture fractional(Layout(80, false, false, 1.3));
    const auto fraction_request = fractional.Request();
    const auto fraction_plan = fractional.scene.PreparePopupSurface(
        fraction_request, Configure(fraction_request, false, 240, 182));
    assert(fraction_plan);
    const auto *fraction_panel = fraction_plan->input_snapshot->Find(fraction_request.active_node);
    assert(contracts::ContourBounds(*fraction_panel->contour) == fraction_plan->window_geometry);

    Fixture narrow(Layout(80, false, false, 16, 30), {180, 360});
    assert(narrow.scene.Bounds(narrow.Request().active_node).width < 240);
    assert(narrow.Request().desired_body.width == 240);
}

void UnsupportedAndBudget()
{
    Fixture editor(Layout(80, true));
    const auto request = editor.Request();
    const auto before = editor.scene.InputGeometry();
    std::string diagnostic;
    assert(!editor.scene.PreparePopupSurface(request, Configure(request), &diagnostic));
    assert(diagnostic.find("text editors") != std::string::npos);
    assert(editor.scene.InputGeometry() == before &&
           editor.scene.PopupToken() == request.popup_token);

    Fixture large;
    const auto normal = large.Request();
    assert(large.scene.SetProperty(normal.active_node, DslProperty::ShadowBlur, 512.0));
    assert(large.scene.Build({17}));
    const auto huge = large.Request();
    assert(!large.scene.PreparePopupSurface(huge, Configure(huge, false, 1500, 1500), &diagnostic));
    assert(diagnostic.find("buffer") != std::string::npos);
}

void ScrollLocalLayout()
{
    Fixture f(Layout(80, false, true));
    const auto shown = f.scene.InputGeometry();
    contracts::NodeId scroll;
    for (const auto &node : shown->nodes) {
        if (node.id && node.children.size() == 1 &&
            shown->Find(node.children.front())->action.empty() && node.clip &&
            node.id != f.Request().active_node) {
            scroll = node.id;
        }
    }
    assert(scroll && f.scene.ScrollTo(scroll, 300));
    assert(f.scene.Build({17}));
    const auto request = f.Request();
    const auto original = f.scene.InputGeometry();
    const auto metrics = f.scene.ScrollInfo(scroll);
    assert(metrics && metrics->offset > 0);
    const auto expanded = f.scene.PreparePopupSurface(request, Configure(request, false, 240, 520));
    assert(expanded);
    const auto *local = expanded->input_snapshot->Find(scroll);
    assert(local && local->scroll_offset == 0);
    assert(f.scene.ScrollInfo(scroll)->offset == metrics->offset);
    assert(f.scene.InputGeometry() == original);
    const auto smaller = f.scene.PreparePopupSurface(request, Configure(request, false, 240, 136));
    assert(smaller && smaller->input_snapshot->Find(scroll)->scroll_offset > 0);
}

void ThemeMetadataWithoutPainting()
{
    Fixture f;
    contracts::ThemeSnapshot theme;
    theme.id = "popup-plan";
    theme.name = "Popup Plan";
    theme.generation = 1;
    assert(f.scene.ApplyTheme(theme));
    assert(f.scene.Build({17}));
    f.scene.AcknowledgeComposite();
    const auto before = f.Request();
    const auto initial = f.scene.PreparePopupSurface(before, Configure(before));
    assert(initial);
    const auto pixels = f.scene.PixelsRevision();
    const auto stats = f.scene.GetRenderStats();

    theme.generation = 2;
    assert(f.scene.ApplyTheme(theme));
    assert(f.scene.PixelsRevision() == pixels);
    assert(!Has(f.scene.PendingDirty(), Dirty::Layout) &&
           !Has(f.scene.PendingDirty(), Dirty::Paint));
    assert(!f.scene.PreparePopupSurface(before, Configure(before)));
    const auto current = f.Request(); // Metadata-only Host packets do not call Build.
    assert(f.scene.GetRenderStats() == stats && f.scene.PixelsRevision() == pixels);
    assert(!f.scene.Build({17}));
    const auto after_stats = f.scene.GetRenderStats();
    assert(after_stats.builds == stats.builds && after_stats.layouts == stats.layouts);
    assert(after_stats.build_calls == stats.build_calls + 1);

    assert(f.Request().source == current.source);
    assert(current.theme_generation == 2 && current.pixels_revision == before.pixels_revision);
    assert(current.source != before.source && current.popup_token == before.popup_token);
    const auto prepared = f.scene.PreparePopupSurface(current, Configure(current));
    assert(prepared && prepared->display_list->commands == initial->display_list->commands);
    assert(!f.scene.PreparePopupSurface(before, Configure(before)));
}

Blueprint MenuLayout()
{
    auto layout = Layout();
    auto &menu = layout.children.back();
    menu.kind = Kind::Menu;
    menu.children.clear();
    Blueprint content;
    content.kind = Kind::Column;
    auto item = Target("sort", 0, 36);
    item.kind = Kind::MenuItem;
    content.children.push_back(std::move(item));
    menu.children.push_back(std::move(content));

    auto submenu = Box(240, 180);
    submenu.kind = Kind::Menu;
    submenu.contour_recipe = AttachedPanelRecipe{12.0, 28.0, 16.0, ContourFallback::Detached};
    submenu.properties.push_back({DslProperty::PopupFor, std::string("sort")});
    submenu.properties.push_back({DslProperty::Background, white});
    auto back = Box(0, 36);
    back.kind = Kind::MenuBack;
    submenu.children.push_back(std::move(back));
    layout.children.push_back(std::move(submenu));
    return layout;
}

void MenuReplacementAndBack()
{
    Fixture f(MenuLayout());
    const auto root = f.Request();
    assert(root.trigger == f.anchor);
    assert(f.scene.PreparePopupSurface(root, Configure(root)));
    const auto sort = Action(*f.scene.InputGeometry(), "sort");
    assert(f.scene.OpenPopup(sort));
    assert(!f.scene.PreparePopupSurface(root, Configure(root)));
    assert(f.scene.Build({17}));
    const auto child = f.Request();
    assert(child.active_node != root.active_node && child.popup_token != root.popup_token);
    assert(child.trigger == root.trigger && child.anchor == root.anchor);
    assert(!f.scene.IsVisible(root.active_node));
    assert(f.scene.PreparePopupSurface(child, Configure(child)));

    f.scene.ApplyInputSnapshot(f.scene.InputGeometry());
    const auto result = f.scene.HandleInput(
        contracts::KeyEvent{{17}, 0x50, contracts::ButtonState::Pressed, false, 1, {0, 0, 0}, {}},
        f.scene.InputGeometry());
    assert(!result.activation && f.scene.PopupToken() != child.popup_token);
    assert(f.scene.Build({17}));
    const auto restored = f.Request();
    assert(restored.active_node == root.active_node && restored.trigger == root.trigger);
    assert(restored.popup_token != root.popup_token && restored.popup_token != child.popup_token);
    assert(f.scene.PreparePopupSurface(restored, Configure(restored)));
    assert(!f.scene.PreparePopupSurface(child, Configure(child)));
    assert(!f.scene.PreparePopupSurface(root, Configure(root)));
}
} // namespace

int main()
{
    GeometryAndPurity();
    MetadataAndLifetime();
    ParentAndFallback();
    UnsupportedAndBudget();
    ScrollLocalLayout();
    ThemeMetadataWithoutPainting();
    MenuReplacementAndBack();
}
