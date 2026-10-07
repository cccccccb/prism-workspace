#include "prism/contracts/contour.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include "prism/contracts/rounded_region.hpp"
#include "prism/runtime/contour_recipe.hpp"
#include "prism/runtime/scene.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::Color white{255, 255, 255, 255};

ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font / 2, font};
}

contracts::ThemeSnapshot Theme(std::uint64_t generation = 1, double radius = 12, double width = 24,
                               double height = 16)
{
    contracts::ThemeSnapshot theme;
    theme.id = "attached-panel-fixture";
    theme.name = "Attached Panel Fixture";
    theme.generation = generation;
    theme.numbers = {{"panel_radius", radius}, {"neck_width", width}, {"neck_height", height}};
    theme.colors = {{"surface", white}};
    theme.controls.hover.a = 0;
    theme.controls.focus.a = 0;
    return theme;
}

Blueprint Box(double width, double height)
{
    Blueprint box;
    box.properties = {{DslProperty::Width, width}, {DslProperty::Height, height}};
    return box;
}

Blueprint Target(std::string action, double width = 0, double height = 0)
{
    auto node = Box(width, height);
    node.kind = Kind::InteractionTarget;
    node.properties.push_back({DslProperty::Action, std::move(action)});
    return node;
}

AttachedPanelRecipe Recipe(contracts::PanelNeckShape shape = contracts::PanelNeckShape::SoftTab)
{
    return {ContourThemeNumber{"panel_radius"}, ContourThemeNumber{"neck_width"},
            ContourThemeNumber{"neck_height"}, ContourFallback::Detached, shape};
}

Blueprint Layout(double lead, double left,
                 contracts::PanelNeckShape shape = contracts::PanelNeckShape::SoftTab)
{
    Blueprint root;
    root.children.push_back(Target("background"));

    Blueprint flow;
    flow.kind = Kind::Column;
    auto spacer = Box(0, lead);
    spacer.bindings.push_back({"lead", DslProperty::Height});
    flow.children.push_back(std::move(spacer));
    auto row = Box(0, 28);
    row.kind = Kind::Row;
    auto prefix = Box(left, 28);
    prefix.bindings.push_back({"left", DslProperty::Width});
    row.children = {std::move(prefix), Target("sound", 48, 28)};
    flow.children.push_back(std::move(row));
    auto slot = Box(20, 20);
    slot.region = "future";
    slot.children.push_back(Box(10, 10));
    flow.children.push_back(std::move(slot));
    root.children.push_back(std::move(flow));

    auto popup = Box(160, 104);
    popup.kind = Kind::Popup;
    popup.contour_recipe = Recipe(shape);
    popup.properties.push_back({DslProperty::PopupFor, std::string("sound")});
    popup.properties.push_back({DslProperty::Padding, 12.0});
    popup.properties.push_back({DslProperty::BackdropBlur, 6.0});
    popup.theme_refs.push_back({"surface", DslProperty::Background});
    popup.children.push_back(Target("inside", 100, 24));
    root.children.push_back(std::move(popup));
    return root;
}

contracts::DisplayList Build(Scene &scene)
{
    const auto list = scene.Build({1});
    assert(list);
    contracts::ValidateDisplayList(*list);
    scene.AcknowledgeComposite();
    return *list;
}

contracts::NodeId FindAction(const InputSnapshot &snapshot, std::string_view action)
{
    for (const auto &node : snapshot.nodes) {
        if (node.id && node.action == action) {
            return node.id;
        }
    }
    assert(false);
    return {};
}

const InputSnapshotNode &Panel(const InputSnapshot &snapshot)
{
    const InputSnapshotNode *panel = nullptr;
    for (const auto &node : snapshot.nodes) {
        if (node.contour && node.visible && node.interactive) {
            assert(!panel);
            panel = &node;
        }
    }
    assert(panel && panel->interactive && panel->clip && panel->radius == 0);
    return *panel;
}

bool InputContains(std::span<const contracts::SurfaceInputRegion> regions,
                   contracts::LogicalPoint point)
{
    for (const auto &region : regions) {
        if (contracts::RoundedRegionContains(point, region)) {
            return true;
        }
    }
    return false;
}

InteractionResult Button(Scene &scene, const std::shared_ptr<const InputSnapshot> &snapshot,
                         contracts::LogicalPoint point, bool down)
{
    return scene.HandleInput(contracts::PointerButtonEvent{{1},
                                                           point,
                                                           contracts::PointerButton::Primary,
                                                           down ? contracts::ButtonState::Pressed
                                                                : contracts::ButtonState::Released,
                                                           0,
                                                           1,
                                                           {1, 1, 1}},
                             snapshot);
}

struct Fixture {
    Scene scene;
    contracts::NodeId anchor;

    Fixture(Blueprint layout, contracts::LogicalSize viewport,
            contracts::ThemeSnapshot theme = Theme())
        : scene(std::move(layout), Shape, {}, std::move(theme))
    {
        assert(scene.SetViewport(viewport));
        Build(scene);
        anchor = FindAction(*scene.InputGeometry(), "sound");
        assert(scene.OpenPopup(anchor));
        Build(scene);
        scene.ApplyInputSnapshot(scene.InputGeometry());
    }

    Fixture(double lead = 40, double left = 100, contracts::LogicalSize viewport = {400, 300},
            contracts::ThemeSnapshot theme = Theme())
        : Fixture(Layout(lead, left), viewport, std::move(theme))
    {
    }
};

contracts::LogicalPoint NeckPoint(const InputSnapshotNode &panel, bool above,
                                  const contracts::Contour *exclude = nullptr)
{
    const auto extent = contracts::ContourBounds(*panel.contour);
    const double y = above ? (panel.bounds.y + panel.bounds.height + extent.y + extent.height) / 2
                           : (panel.bounds.y + extent.y) / 2;
    std::optional<double> first;
    double last = 0;
    for (int x = static_cast<int>(std::floor(extent.x)); x < extent.x + extent.width; ++x) {
        const contracts::LogicalPoint point{x + .5, y};
        if (contracts::ContourContains(*panel.contour, point) &&
            (!exclude || !contracts::ContourContains(*exclude, point))) {
            if (!first) {
                first = point.x;
            }
            last = point.x;
        }
    }
    assert(first);
    // For a difference mask its two wings can be disconnected.
    return {exclude ? *first : (*first + last) / 2, y};
}

void CheckConsumers(Fixture &fixture)
{
    auto &scene = fixture.scene;
    const auto snapshot = scene.InputGeometry();
    const auto &panel = Panel(*snapshot);
    contracts::ValidateContour(*panel.contour);
    const auto effects = scene.SurfaceEffects();
    assert(effects.size() == 1 && effects.front().contour == *panel.contour);
    assert(effects.front().bounds == contracts::ContourBounds(*panel.contour));
    assert(effects.front().corner_radius == 0 && effects.front().blur_radius == 6);
    contracts::ValidateSurfaceEffectRegion(effects.front());

    const auto inside = FindAction(*snapshot, "inside");
    const auto content = scene.Bounds(inside);
    assert(content.x == panel.bounds.x + 12 && content.y == panel.bounds.y + 12);
    assert(content.height == 24);
    const auto &regions = scene.InputRegions();
    assert(!regions.empty());
    for (const auto &region : regions) {
        assert(contracts::ValidRoundedRegion(region));
    }
    const auto mask = contracts::RasterizeContourIntersection(
        std::span<const contracts::Contour>(panel.contour.get(), 1),
        {0, 0, snapshot->viewport.width, snapshot->viewport.height});
    for (const auto &rect : mask) {
        for (int y = static_cast<int>(rect.y); y < rect.y + rect.height; ++y) {
            for (int x = static_cast<int>(rect.x); x < rect.x + rect.width; ++x) {
                assert(InputContains(regions, {x + .5, y + .5}));
            }
        }
    }
}

void CheckPlacement()
{
    for (const auto [lead, above] : {std::pair{40.0, false}, std::pair{220.0, true}}) {
        Fixture fixture(lead);
        auto &scene = fixture.scene;
        const auto snapshot = scene.InputGeometry();
        const auto &panel = Panel(*snapshot);
        const auto anchor = scene.Bounds(fixture.anchor);
        const auto extent = contracts::ContourBounds(*panel.contour);
        assert(panel.bounds.width == 160 && panel.bounds.height == 104);
        assert(extent.width == 160 && extent.height == 120);
        if (above) {
            assert(extent.y == panel.bounds.y);
            assert(extent.y + extent.height == anchor.y - 8);
        } else {
            assert(extent.y == anchor.y + anchor.height + 8);
            assert(panel.bounds.y == extent.y + 16);
        }
        const auto neck = NeckPoint(panel, above);
        assert(neck.x >= anchor.x && neck.x <= anchor.x + anchor.width);
        assert(scene.HitTest(neck)->node == panel.id);
        assert(scene.HitTest(neck, *snapshot)->node == panel.id);
        assert(InputContains(scene.InputRegions(), neck));
        const auto token = scene.PopupToken();
        assert(!Button(scene, snapshot, neck, true).activation);
        assert(!Button(scene, snapshot, neck, false).activation);
        assert(scene.PopupToken() == token);
        CheckConsumers(fixture);
    }
}

contracts::LogicalPoint SingleApex(const InputSnapshotNode &panel, bool above)
{
    const auto extent = contracts::ContourBounds(*panel.contour);
    const auto tip_y = above ? extent.y + extent.height : extent.y;
    std::optional<contracts::LogicalPoint> apex;
    for (const auto point : panel.contour->points) {
        if (point.y == tip_y) {
            assert(!apex); // A rounded triangle has one extremum, never a flat cap.
            apex = point;
        }
    }
    assert(apex);
    return *apex;
}

void CheckRoundedTriangle()
{
    constexpr auto shape = contracts::PanelNeckShape::RoundedTriangle;
    for (const auto [lead, above] : {std::pair{40.0, false}, std::pair{220.0, true}}) {
        for (const auto radius : {12.0, 0.0}) {
            Fixture fixture(Layout(lead, 100, shape), {400, 300}, Theme(1, radius, 40, 14));
            auto &scene = fixture.scene;
            const auto before = scene.InputGeometry();
            const auto &panel = Panel(*before);
            const auto panel_id = panel.id;
            const auto contour = panel.contour;
            const auto extent = contracts::ContourBounds(*contour);
            const auto apex = SingleApex(panel, above);
            const auto anchor = scene.Bounds(fixture.anchor);
            assert(extent.height == 118 && panel.bounds.height == 104);
            assert(apex.x >= anchor.x && apex.x <= anchor.x + anchor.width);
            assert(scene.HitTest(apex)->node == panel_id);
            assert(scene.HitTest(apex, *before)->node == panel_id);
            CheckConsumers(fixture);

            Fixture soft(Layout(lead, 100), {400, 300}, Theme(1, radius, 40, 14));
            assert(*Panel(*soft.scene.InputGeometry()).contour != *contour);

            // Theme dimensions can change while the authored triangle identity
            // and popup lifetime survive. Previous input stays immutable.
            const auto token = scene.PopupToken();
            assert(scene.ApplyTheme(Theme(2, radius, 64, 18)));
            Build(scene);
            const auto current = scene.InputGeometry();
            const auto &changed = Panel(*current);
            assert(current->version > before->version && changed.id == panel_id);
            assert(changed.contour != contour && before->Find(panel_id)->contour == contour);
            assert(contracts::ContourBounds(*changed.contour).height == 122);
            SingleApex(changed, above);
            assert(scene.PopupToken() == token);
            CheckConsumers(fixture);

            const auto revision = scene.TransactionRevision();
            const auto pixels = scene.PixelsRevision();
            std::string diagnostic;
            assert(!scene.ApplyTheme(Theme(3, radius, 257, 18), &diagnostic));
            assert(!diagnostic.empty() && scene.ThemeGeneration() == 2);
            assert(scene.InputGeometry() == current && scene.PopupToken() == token);
            assert(scene.TransactionRevision() == revision && scene.PixelsRevision() == pixels);
        }
    }
}

void CheckAvoidanceAndDetached()
{
    Fixture right(40, 350);
    const auto &right_panel = Panel(*right.scene.InputGeometry());
    const auto right_extent = contracts::ContourBounds(*right_panel.contour);
    assert(right_panel.bounds.x + right_panel.bounds.width <= 392);
    assert(right_extent.height == 120);
    const auto right_neck = NeckPoint(right_panel, false);
    const auto anchor = right.scene.Bounds(right.anchor);
    assert(right_neck.x >= anchor.x && right_neck.x <= anchor.x + anchor.width);
    CheckConsumers(right);

    Fixture edge(40, 10, {120, 180});
    const auto &edge_panel = Panel(*edge.scene.InputGeometry());
    assert(edge_panel.bounds.width == 104);
    assert(contracts::ContourBounds(*edge_panel.contour) == edge_panel.bounds);
    CheckConsumers(edge);

    // A broad corner leaves no safe neck center over this leftmost anchor.
    // Detached fallback preserves the body and already chosen placement gap.
    Fixture narrow(40, 1, {400, 300}, Theme(1, 80, 24, 16));
    const auto &narrow_panel = Panel(*narrow.scene.InputGeometry());
    assert(contracts::ContourBounds(*narrow_panel.contour) == narrow_panel.bounds);
    const auto narrow_anchor = narrow.scene.Bounds(narrow.anchor);
    assert(narrow_panel.bounds.y == narrow_anchor.y + narrow_anchor.height + 24);
    CheckConsumers(narrow);
}

void CheckExactAnchorClip()
{
    auto layout = Layout(40, 100);
    auto &row = layout.children[1].children[1];
    row.properties.push_back({DslProperty::Clip, true});
    row.contour = contracts::Contour{{{0, 0}, {400, 0}, {400, 6}, {10, 6}, {10, 28}, {0, 28}}};
    Fixture fixture(std::move(layout), {400, 300});
    const auto snapshot = fixture.scene.InputGeometry();
    const auto *anchor = snapshot->Find(fixture.anchor);
    const auto *clip = snapshot->Find(anchor->parent);
    assert(clip && clip->clip && clip->contour);
    const contracts::LogicalPoint center{anchor->bounds.x + anchor->bounds.width / 2,
                                         anchor->bounds.y + anchor->bounds.height / 2};
    assert(contracts::RoundedRegionContains(center, {clip->bounds, 0}));
    assert(!contracts::ContourContains(*clip->contour, center));
    // Only the anchor's upper strip is visible. Its bounding rectangle still
    // permits placement, but the actual midpoint is outside the ancestor clip.
    assert(contracts::ContourContains(*clip->contour, {center.x, anchor->bounds.y + 2}));
    const auto &panel = Panel(*snapshot);
    assert(contracts::ContourBounds(*panel.contour) == panel.bounds);
    assert(panel.bounds.y == anchor->bounds.y + anchor->bounds.height + 24);
    CheckConsumers(fixture);
}

void CheckInactiveThemeChange()
{
    Scene scene(Layout(40, 100), Shape, {}, Theme());
    assert(scene.SetViewport({400, 300}));
    Build(scene);
    const auto snapshot = scene.InputGeometry();
    const auto pixels = scene.PixelsRevision();
    const auto layouts = scene.GetRenderStats().layouts;
    assert(!scene.PopupToken());
    assert(scene.ApplyTheme(Theme(2, 12, 24, 24)));
    assert(scene.ThemeGeneration() == 2 && scene.PixelsRevision() == pixels);
    assert(!Has(scene.PendingDirty(), Dirty::Layout) && !Has(scene.PendingDirty(), Dirty::Paint));
    assert(!scene.Build({1}));
    assert(scene.GetRenderStats().layouts == layouts && scene.InputGeometry() == snapshot);
    assert(scene.OpenPopup(FindAction(*snapshot, "sound")));
    Build(scene);
    assert(contracts::ContourBounds(*Panel(*scene.InputGeometry()).contour).height == 128);
}

void CheckHiddenScalarRadius()
{
    Fixture fixture;
    auto &scene = fixture.scene;
    const auto &panel = Panel(*scene.InputGeometry());
    const auto panel_id = panel.id;
    const auto expected = *panel.contour;
    assert(scene.ClosePopup());
    Build(scene);
    const auto closed = scene.InputGeometry();
    const auto version = closed->version;
    assert(closed->Find(panel_id)->radius == 0);

    assert(scene.SetProperty(panel_id, DslProperty::Radius, 31.0));
    scene.Build({1});
    scene.AcknowledgeComposite();
    assert(scene.InputGeometry() == closed && scene.InputGeometry()->version == version);
    assert(scene.InputGeometry()->Find(panel_id)->radius == 0);
    assert(scene.OpenPopup(fixture.anchor));
    Build(scene);
    assert(*Panel(*scene.InputGeometry()).contour == expected);
}

void CheckPaintReuseAndFrozenInput()
{
    Fixture fixture;
    auto &scene = fixture.scene;
    const auto original = scene.InputGeometry();
    const auto panel_id = Panel(*original).id;
    const auto contour = original->Find(panel_id)->contour;
    assert(scene.SetBackground(panel_id, {20, 80, 140, 255}));
    const auto layouts = scene.GetRenderStats().layouts;
    const auto paint = Build(scene);
    assert(scene.GetRenderStats().layouts == layouts);
    assert(scene.InputGeometry() == original);
    assert(scene.InputGeometry()->Find(panel_id)->contour == contour);
    int fills = 0;
    for (const auto &command : paint.commands) {
        if (const auto *fill = std::get_if<contracts::FillContour>(&command)) {
            ++fills;
            assert(fill->contour == *contour && fill->color == contracts::Color(20, 80, 140, 255));
        }
    }
    assert(fills == 1);

    assert(scene.ApplyTheme(Theme(2, 12, 8, 16)));
    Build(scene);
    const auto changed = scene.InputGeometry();
    assert(changed->version > original->version);
    assert(changed->Find(panel_id)->contour != contour);
    const auto old_neck =
        NeckPoint(*original->Find(panel_id), false, changed->Find(panel_id)->contour.get());
    assert(scene.HitTest(old_neck, *original)->node == panel_id);
    const auto current_hit = scene.HitTest(old_neck);
    assert(!current_hit || current_hit->node != panel_id);
    assert(original->Find(panel_id)->contour == contour);

    assert(scene.ApplyTheme(Theme(3, 0, 24, 16)));
    Build(scene);
    const auto &square = Panel(*scene.InputGeometry());
    assert(square.contour->points.size() <= 12);
    assert(*square.contour != *contour);
    assert(contracts::ContourBounds(*square.contour).height == 120);
    CheckConsumers(fixture);
}

void CheckThemeRollback()
{
    Fixture fixture;
    auto &scene = fixture.scene;
    const auto snapshot = scene.InputGeometry();
    const auto panel_id = Panel(*snapshot).id;
    const auto bounds = scene.Bounds(panel_id);
    const auto token = scene.PopupToken();
    const auto stats = scene.GetRenderStats();
    const auto revision = scene.TransactionRevision();
    const auto pixels = scene.PixelsRevision();
    const auto dirty = scene.PendingDirty();
    const auto effects = scene.SurfaceEffects();
    const auto regions = scene.InputRegions();
    const auto *region_data = scene.InputRegions().data();
    const auto state = scene.State(fixture.anchor);
    const auto neck = NeckPoint(Panel(*snapshot), false);
    std::vector<contracts::ThemeSnapshot> invalid{Theme(2, -1), Theme(2, 257), Theme(2, 12, 257),
                                                  Theme(2, 12, 24, 49)};
    auto missing = Theme(2);
    missing.numbers.pop_back();
    invalid.push_back(std::move(missing));
    auto wrong_type = Theme(2);
    wrong_type.numbers.pop_back();
    wrong_type.colors.push_back({"neck_height", white});
    invalid.push_back(std::move(wrong_type));

    for (const auto &theme : invalid) {
        std::string diagnostic;
        assert(!scene.ApplyTheme(theme, &diagnostic) && !diagnostic.empty());
        assert(scene.ThemeGeneration() == 1 && scene.TransactionRevision() == revision);
        assert(scene.GetRenderStats() == stats && scene.PixelsRevision() == pixels);
        assert(scene.PendingDirty() == dirty && scene.Bounds(panel_id) == bounds);
        assert(scene.InputGeometry() == snapshot && scene.PopupToken() == token);
        assert(scene.State(fixture.anchor) == state && scene.SurfaceEffects() == effects);
        assert(scene.InputRegions() == regions && scene.InputRegions().data() == region_data);
        assert(scene.HitTest(neck)->node == panel_id);
        assert(scene.HitTest(neck, *snapshot)->node == panel_id);
    }
    assert(scene.ApplyTheme(Theme(2, 8, 32, 24)));
    Build(scene);
    assert(contracts::ContourBounds(*Panel(*scene.InputGeometry()).contour).height == 128);
}

void CheckOutsidePress()
{
    Fixture fixture;
    auto &scene = fixture.scene;
    const auto snapshot = scene.InputGeometry();
    const auto &panel = Panel(*snapshot);
    const auto neck = NeckPoint(panel, false);
    const contracts::LogicalPoint outside{panel.bounds.x + 1, neck.y};
    assert(!contracts::ContourContains(*panel.contour, outside));
    const auto background = FindAction(*snapshot, "background");
    const auto outside_hit = scene.HitTest(outside);
    assert(!outside_hit || outside_hit->node == background);
    assert(!Button(scene, snapshot, outside, true).activation);
    assert(!scene.PopupToken());
    assert(!Button(scene, snapshot, outside, false).activation);
    assert(!scene.State(background).pressed && !scene.State(background).captured);
    Build(scene);
    const auto closed = scene.InputGeometry();
    Button(scene, closed, outside, true);
    const auto activation = Button(scene, closed, outside, false);
    assert(activation.activation && activation.activation->action == "background");
}

void CheckProjectionAndProvenance()
{
    Fixture fixture;
    auto &scene = fixture.scene;
    const auto before = scene.InputGeometry();
    const auto panel_id = Panel(*before).id;
    assert(scene.Preflight({{"left", 180.0}}));
    Build(scene);
    assert(scene.InputGeometry()->Find(panel_id)->contour != before->Find(panel_id)->contour);
    const RegionUpdate update{"future", Box(12, 12)};
    auto blueprint = scene.RegionBlueprint(std::span(&update, 1));
    assert(blueprint.children.back().contour_recipe == Recipe());
    assert(!blueprint.children.back().contour);
    // Literal and themed radius resolve equally, but are different provenance.
    blueprint.children.back().contour_recipe->radius = 12.0;
    Scene candidate(std::move(blueprint), Shape, {}, Theme());
    const auto revision = scene.TransactionRevision();
    const auto pixels = scene.PixelsRevision();
    const auto snapshot = scene.InputGeometry();
    std::string diagnostic;
    assert(!scene.MountRegions(std::span(&update, 1), {}, candidate, revision, &diagnostic));
    assert(!diagnostic.empty() && candidate.RootId());
    assert(scene.TransactionRevision() == revision && scene.PixelsRevision() == pixels);
    assert(scene.InputGeometry() == snapshot && !scene.RegionMounted("future"));
    assert(scene.MountRegions(std::span(&update, 1), {}));
    Build(scene);
    assert(scene.RegionMounted("future"));
    assert(scene.InputGeometry()->Find(panel_id)->contour == snapshot->Find(panel_id)->contour);
}

void CheckShapeProvenance()
{
    constexpr auto shape = contracts::PanelNeckShape::RoundedTriangle;
    Fixture fixture(Layout(40, 100, shape), {400, 300}, Theme(1, 12, 40, 14));
    auto &scene = fixture.scene;
    const RegionUpdate update{"future", Box(12, 12)};
    auto blueprint = scene.RegionBlueprint(std::span(&update, 1));
    assert(blueprint.children.back().contour_recipe == Recipe(shape));
    blueprint.children.back().contour_recipe->neck_shape = contracts::PanelNeckShape::SoftTab;
    Scene candidate(std::move(blueprint), Shape, {}, Theme(1, 12, 40, 14));

    const auto revision = scene.TransactionRevision();
    const auto pixels = scene.PixelsRevision();
    const auto snapshot = scene.InputGeometry();
    const auto token = scene.PopupToken();
    std::string diagnostic;
    assert(!scene.MountRegions(std::span(&update, 1), {}, candidate, revision, &diagnostic));
    assert(!diagnostic.empty() && candidate.RootId());
    assert(scene.TransactionRevision() == revision && scene.PixelsRevision() == pixels);
    assert(scene.InputGeometry() == snapshot && scene.PopupToken() == token);
    assert(!scene.RegionMounted("future"));

    assert(scene.MountRegions(std::span(&update, 1), {}));
    Build(scene);
    assert(scene.RegionMounted("future") && scene.PopupToken() == token);
    assert(Panel(*scene.InputGeometry()).contour == Panel(*snapshot).contour);
}
} // namespace

int main()
{
    CheckPlacement();
    CheckRoundedTriangle();
    CheckAvoidanceAndDetached();
    CheckExactAnchorClip();
    CheckInactiveThemeChange();
    CheckHiddenScalarRadius();
    CheckPaintReuseAndFrozenInput();
    CheckThemeRollback();
    CheckOutsidePress();
    CheckProjectionAndProvenance();
    CheckShapeProvenance();
}
