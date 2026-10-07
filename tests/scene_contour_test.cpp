#include "prism/contracts/contour.hpp"
#include "prism/contracts/display_list_validation.hpp"
#include "prism/contracts/rounded_region.hpp"
#include "prism/render_skia/raster_renderer.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::Color white{255, 255, 255, 255};
constexpr contracts::Color red{220, 30, 40, 255};

ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font / 2, font};
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

const contracts::FillContour &FindFill(const contracts::DisplayList &list,
                                       contracts::Color color = white)
{
    for (const auto &command : list.commands) {
        if (const auto *fill = std::get_if<contracts::FillContour>(&command);
            fill && fill->color == color) {
            return *fill;
        }
    }
    assert(false);
    throw std::logic_error("Missing contour fill");
}

contracts::Contour Translate(contracts::Contour contour, double x, double y)
{
    for (auto &point : contour.points) {
        point.x += x;
        point.y += y;
    }
    contracts::ValidateContour(contour);
    return contour;
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

std::vector<std::uint32_t> Render(const contracts::DisplayList &list, int width, int height)
{
    render_skia::RasterRenderer renderer("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    std::vector<std::uint32_t> pixels(width * height);
    assert(renderer.Render(list, pixels.data(), width, height, width * 4));
    return pixels;
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

InteractionResult Click(Scene &scene, const std::shared_ptr<const InputSnapshot> &snapshot,
                        contracts::LogicalPoint point)
{
    Button(scene, snapshot, point, true);
    return Button(scene, snapshot, point, false);
}

void CheckCanonicalGeometry()
{
    constexpr auto source = R"(
Card(spacing:0) {
 InteractionTarget(action:"panel",width:48,height:48,inset:0.253,
                   background:#FFFFFFFF,cornerRadius:64,backdropBlur:12,clip:true) {
  Contour(space:"local") {
   Move(x:4,y:8) Line(x:26,y:8) Line(x:26,y:0) Line(x:38,y:0)
   Line(x:38,y:8) Line(x:60,y:8) Line(x:60,y:48) Line(x:40,y:48)
   Line(x:40,y:28) Line(x:24,y:28) Line(x:24,y:48) Line(x:4,y:48)
  }
 }
}
)";
    const auto blueprint = ParseBlueprint(source);
    assert(blueprint.children.size() == 1 && blueprint.children.front().contour);
    const auto local = *blueprint.children.front().contour;
    Scene scene(blueprint, Shape);
    assert(scene.SetViewport({96, 80}));
    const auto list = Build(scene);
    const auto snapshot = scene.InputGeometry();
    assert(snapshot);
    const auto id = FindAction(*snapshot, "panel");
    const auto *node = snapshot->Find(id);
    assert(node && node->contour && node->radius == 0);
    assert(scene.Bounds(id) == contracts::LogicalRect(0.253, 0.253, 48, 48));
    const auto expected = Translate(local, 0.25390625, 0.25390625);
    assert(*node->contour == expected && FindFill(list).contour == expected);
    assert(contracts::ContourBounds(expected).x + contracts::ContourBounds(expected).width >
           scene.Bounds(id).x + scene.Bounds(id).width);

    int clips = 0;
    for (const auto &command : list.commands) {
        if (const auto *clip = std::get_if<contracts::PushClipContour>(&command)) {
            ++clips;
            assert(clip->contour == expected);
        }
        assert(!std::holds_alternative<contracts::FillRoundedRect>(command));
        assert(!std::holds_alternative<contracts::PushClipRoundedRect>(command));
    }
    assert(clips == 1);
    const auto effects = scene.SurfaceEffects();
    assert(effects.size() == 1 && effects.front().contour == expected);
    assert(effects.front().bounds == contracts::ContourBounds(expected));
    assert(effects.front().corner_radius == 0 && effects.front().blur_radius == 12);
    const auto regions = scene.InputRegions();
    assert(!regions.empty());
    for (const auto &region : regions) {
        assert(region.corner_radius == 0);
    }
    const auto pixels = Render(list, 96, 80);
    for (int y = 0; y < 80; ++y) {
        for (int x = 0; x < 96; ++x) {
            const contracts::LogicalPoint point{x + 0.5, y + 0.5};
            const bool inside = contracts::ContourContains(expected, point);
            const auto live = scene.HitTest(point);
            const auto frozen = scene.HitTest(point, *snapshot);
            assert(bool(live) == inside && bool(frozen) == inside);
            if (inside) {
                assert(live->node == id && frozen->node == id);
            }
            assert(InputContains(regions, point) == inside);
            const auto alpha = pixels[y * 96 + x] >> 24;
            if (alpha == 255) {
                assert(inside);
            }
            if (alpha == 0) {
                assert(!inside);
            }
        }
    }
    // The concavity produces two input spans; its gap stays transparent and inert.
    assert(scene.ActionAt({12.5, 40.5}) == "panel");
    assert(!scene.ActionAt({32.5, 40.5}));
    assert(scene.ActionAt({52.5, 40.5}) == "panel");
    assert((pixels[40 * 96 + 32] >> 24) == 0);

    assert(scene.SetProperty(id, DslProperty::Radius, 1.0));
    const auto same = Build(scene);
    assert(FindFill(same).contour == expected);
    assert(scene.InputGeometry() == snapshot);
    assert(scene.InputGeometry()->Find(id)->contour == node->contour);
}

void CheckAncestorIntersection()
{
    Scene scene(ParseBlueprint(R"(
Card(spacing:0,clip:true,cornerRadius:24) {
 Card(width:80,height:70,spacing:0,clip:true) {
  Contour {
   Move(x:0,y:0) Line(x:80,y:0) Line(x:80,y:30)
   Line(x:50,y:30) Line(x:50,y:70) Line(x:0,y:70)
  }
  InteractionTarget(action:"clipped",width:80,height:70,
                    background:#FFFFFFFF,backdropBlur:$blur) {
   Contour {
    Move(x:8,y:0) Line(x:88,y:0) Line(x:88,y:70) Line(x:8,y:70)
   }
  }
 }
}
)"),
                Shape);
    assert(scene.SetViewport({96, 80}));
    const auto list = Build(scene);
    const auto snapshot = scene.InputGeometry();
    const auto target = FindAction(*snapshot, "clipped");
    const auto *child = snapshot->Find(target);
    const auto *parent = snapshot->Find(child->parent);
    assert(child->contour && parent && parent->contour);
    const auto regions = scene.InputRegions();
    const auto pixels = Render(list, 96, 80);
    const contracts::SurfaceInputRegion rounded{{0, 0, 96, 80}, 24};
    for (int y = 0; y < 80; ++y) {
        for (int x = 0; x < 96; ++x) {
            const contracts::LogicalPoint point{x + 0.5, y + 0.5};
            const bool inside = contracts::ContourContains(*child->contour, point) &&
                                contracts::ContourContains(*parent->contour, point) &&
                                contracts::RoundedRegionContains(point, rounded);
            assert(bool(scene.HitTest(point)) == inside);
            assert(bool(scene.HitTest(point, *snapshot)) == inside);
            assert(InputContains(regions, point) == inside);
            const auto alpha = pixels[y * 96 + x] >> 24;
            if (alpha == 255) {
                assert(inside);
            }
            if (alpha == 0) {
                assert(!inside);
            }
        }
    }
    assert(scene.SurfaceEffects().empty());

    const auto revision = scene.TransactionRevision();
    const auto stats = scene.GetRenderStats();
    const auto pixels_revision = scene.PixelsRevision();
    const auto dirty = scene.PendingDirty();
    std::string diagnostic;
    assert(!scene.Preflight({{"blur", 6.0}}, &diagnostic));
    assert(diagnostic.find("Unsupported backdrop clipping") != std::string::npos);
    assert(scene.TransactionRevision() == revision && scene.GetRenderStats() == stats);
    assert(scene.PixelsRevision() == pixels_revision && scene.PendingDirty() == dirty);
    assert(scene.InputGeometry() == snapshot && scene.SurfaceEffects().empty());
    assert(scene.InputRegions() == regions && !scene.Build({1}));
}

void CheckScrollSnapshots()
{
    Scene scene(ParseBlueprint(R"(
ScrollView(scrollSpeed:1) {
 VStack(spacing:0) {
  InteractionTarget(action:"first",width:60,height:40,background:#FFFFFFFF) {
   Contour {
    Move(x:0,y:0) Line(x:60,y:0) Line(x:60,y:20)
    Line(x:20,y:20) Line(x:20,y:40) Line(x:0,y:40)
   }
  }
  InteractionTarget(action:"second",width:60,height:40,background:#DC1E28FF) {
   Contour { Move(x:0,y:0) Line(x:60,y:0) Line(x:0,y:40) }
  }
  InteractionTarget(action:"third",width:60,height:40,background:#3040C0FF) {
   Contour {
    Move(x:0,y:0) Line(x:60,y:0) Line(x:60,y:40) Line(x:0,y:40)
   }
  }
 }
}
)"),
                Shape);
    assert(scene.SetViewport({80, 64}));
    const auto before_list = Build(scene);
    const auto before = scene.InputGeometry();
    scene.ApplyInputSnapshot(before);
    const auto first = FindAction(*before, "first");
    const auto second = FindAction(*before, "second");
    const auto old_contour = before->Find(second)->contour;
    const auto old_points = *old_contour;
    const auto layouts = scene.GetRenderStats().layouts;
    assert(scene.ActionAt({5, 5}) == "first");

    assert(scene.ScrollTo(scene.RootId(), 40));
    assert(!Has(scene.PendingDirty(), Dirty::Layout));
    assert(scene.InputGeometry() == before);
    assert(scene.ActionAt({5, 5}) == "second");
    assert(scene.HitTest({5, 5}, *before)->node == first);
    assert(*before->Find(second)->contour == old_points);
    assert(!Click(scene, before, {5, 5}).activation);

    const auto after = scene.CaptureInputSnapshot();
    assert(after->version > before->version && after->Find(second)->contour != old_contour);
    assert(*after->Find(second)->contour == Translate(old_points, 0, -40));
    assert(after->Find(scene.RootId())->scroll_offset == 40);
    assert(before->Find(scene.RootId())->scroll_offset == 0);
    const auto after_list = Build(scene);
    assert(scene.GetRenderStats().layouts == layouts);
    assert(FindFill(after_list, red).contour == *after->Find(second)->contour);
    assert(FindFill(before_list, red).contour == old_points);
    scene.ApplyInputSnapshot(after);
    const auto activation = Click(scene, after, {5, 5});
    assert(activation.activation && activation.activation->action == "second");
    const auto pixels = Render(after_list, 80, 64);
    assert((pixels[5 * 80 + 5] >> 24) == 255);
    assert((pixels[20 * 80 + 55] >> 24) == 0);
}

contracts::ThemeSnapshot Theme(std::uint64_t generation, double lead = 10,
                               contracts::Color color = white)
{
    contracts::ThemeSnapshot theme;
    theme.id = "contour-fixture";
    theme.name = "Contour Fixture";
    theme.generation = generation;
    theme.numbers = {{"lead", lead}};
    theme.colors = {{"surface", color}};
    theme.controls.hover.a = 0;
    theme.controls.focus.a = 0;
    return theme;
}

struct RetainedState {
    SceneRenderStats stats;
    std::uint64_t transaction, pixels, theme;
    Dirty dirty;
    contracts::LogicalRect bounds;
    std::shared_ptr<const InputSnapshot> snapshot;

    RetainedState(const Scene &scene, contracts::NodeId id)
        : stats(scene.GetRenderStats()), transaction(scene.TransactionRevision()),
          pixels(scene.PixelsRevision()), theme(scene.ThemeGeneration()),
          dirty(scene.PendingDirty()), bounds(scene.Bounds(id)), snapshot(scene.InputGeometry())
    {
    }

    void Check(const Scene &scene, contracts::NodeId id) const
    {
        assert(scene.GetRenderStats() == stats && scene.TransactionRevision() == transaction);
        assert(scene.PixelsRevision() == pixels && scene.ThemeGeneration() == theme);
        assert(scene.PendingDirty() == dirty && scene.Bounds(id) == bounds);
        assert(scene.InputGeometry() == snapshot);
        assert(scene.InputGeometry()->Find(id)->contour == snapshot->Find(id)->contour);
    }
};

contracts::NodeId FindScroll(const Scene &scene, const InputSnapshot &snapshot)
{
    for (const auto &node : snapshot.nodes) {
        if (node.id && scene.ScrollInfo(node.id)) {
            return node.id;
        }
    }
    assert(false);
    return {};
}

void CheckScrollRegionRollback()
{
    for (bool custom_contour : {false, true}) {
        auto blueprint = ParseBlueprint(R"(
Card {
 ScrollView(width:80,height:64,scrollSpeed:1) {
  VStack(spacing:0) {
   Card(height:10)
   InteractionTarget(action:"open",width:40,height:40,background:#FFFFFFFF,
                     cornerRadius:6,backdropBlur:6) {
    Contour { Move(x:0,y:0) Line(x:40,y:0) Line(x:0,y:40) }
   }
   Card(height:120)
  }
 }
 Popup("open",width:70,height:30,background:#FFFFFFFF) {
  Button("Done",action:"done",width:70,height:30)
 }
}
)");
        if (!custom_contour) {
            blueprint.children.front().children.front().children[1].contour.reset();
        }
        Scene scene(std::move(blueprint), Shape);
        assert(scene.SetViewport({120, 100}));
        Build(scene);
        const auto anchor = FindAction(*scene.InputGeometry(), "open");
        const auto scroll = FindScroll(scene, *scene.InputGeometry());
        assert(scene.OpenPopup(anchor));
        Build(scene);
        const auto done = FindAction(*scene.InputGeometry(), "done");
        const auto bounds = scene.Bounds(done);
        const contracts::LogicalPoint point{bounds.x + 5, bounds.y + 5};
        scene.ApplyInputSnapshot(scene.InputGeometry());
        assert(!Button(scene, scene.InputGeometry(), point, true).activation);
        Build(scene);
        const auto captured = scene.State(done);
        assert(captured.pressed && captured.captured);

        const RetainedState retained(scene, anchor);
        const auto regions = scene.InputRegions();
        const auto *region_data = scene.InputRegions().data();
        const auto effects = scene.SurfaceEffects();
        const auto popup_token = scene.PopupToken();
        const auto metrics = *scene.ScrollInfo(scroll);
        assert(effects.size() == 1 && popup_token && metrics.offset == 0);

        // The coordinates remain legal, but the prospective blur is only
        // partially inside the ScrollView clip. Neither a cropped triangle nor
        // cropped rounded corners are representable by an intact effect.
        assert(!scene.ScrollTo(scroll, 20));
        retained.Check(scene, anchor);
        assert(scene.ScrollInfo(scroll)->offset == metrics.offset);
        assert(scene.ScrollInfo(scroll)->maximum == metrics.maximum);
        assert(scene.PopupToken() == popup_token && scene.State(done) == captured);
        assert(scene.InputRegions() == regions && scene.InputRegions().data() == region_data);
        assert(scene.SurfaceEffects() == effects);

        // A smaller representable movement still succeeds after the rejection.
        assert(scene.ScrollTo(scroll, 4));
        assert(scene.ScrollInfo(scroll)->offset == 4 && !scene.PopupToken());
        assert(!scene.State(done).captured && scene.GetRenderStats() == retained.stats);
        Build(scene);
        assert(scene.SurfaceEffects().size() == 1);
    }
}

void CheckScrollEnvelopeRollback()
{
    Scene scene(ParseBlueprint(R"(
ScrollView(scrollSpeed:1) {
 VStack(spacing:0) {
  InteractionTarget(action:"envelope",width:40,height:40,background:#FFFFFFFF) {
   Contour {
    Move(x:0,y:-8190) Line(x:40,y:-8190)
    Line(x:40,y:-8170) Line(x:0,y:-8170)
   }
  }
  Card(height:120)
 }
}
)"),
                Shape);
    assert(scene.SetViewport({80, 64}));
    Build(scene);
    const auto target = FindAction(*scene.InputGeometry(), "envelope");
    const RetainedState retained(scene, target);
    const auto regions = scene.InputRegions();
    const auto *region_data = scene.InputRegions().data();
    const auto metrics = *scene.ScrollInfo(scene.RootId());

    assert(!scene.ScrollTo(scene.RootId(), 4));
    retained.Check(scene, target);
    assert(scene.ScrollInfo(scene.RootId())->offset == metrics.offset);
    assert(scene.InputRegions() == regions && scene.InputRegions().data() == region_data);
    assert(!scene.ActionAt({5, 5}));
    assert(scene.ScrollTo(scene.RootId(), 1));
    assert(scene.ScrollInfo(scene.RootId())->offset == 1);
    const auto after = scene.CaptureInputSnapshot();
    assert(after->Find(target)->contour && after->version > retained.snapshot->version);
}

void CheckCollapsedScrolledContour()
{
    Scene scene(ParseBlueprint(R"(
ScrollView(scrollSpeed:1) {
 VStack(spacing:0) {
  Card(height:40)
  Card(width:80,height:40,padding:50) {
   InteractionTarget(action:"collapsed",width:20,height:20,background:#FFFFFFFF) {
    Contour { Move(x:0,y:0) Line(x:20,y:0) Line(x:0,y:20) }
   }
  }
  Card(height:80)
 }
}
)"),
                Shape);
    assert(scene.SetViewport({80, 64}));
    Build(scene);
    const auto before = scene.InputGeometry();
    const auto target = FindAction(*before, "collapsed");
    assert(before->Find(target)->bounds.width == 0 && before->Find(target)->bounds.height == 0);
    assert(!before->Find(target)->contour && !scene.ActionAt({55, 55}));
    const auto layouts = scene.GetRenderStats().layouts;

    assert(scene.ScrollTo(scene.RootId(), 40));
    assert(!scene.ActionAt({55, 55}));
    const auto after = scene.CaptureInputSnapshot();
    assert(!after->Find(target)->contour);
    assert(after->Find(target)->bounds.width == 0 && after->Find(target)->bounds.height == 0);
    const auto list = Build(scene);
    assert(scene.GetRenderStats().layouts == layouts &&
           !scene.InputGeometry()->Find(target)->contour);
    for (const auto &command : list.commands) {
        assert(!std::holds_alternative<contracts::FillContour>(command));
    }
}

void CheckTransactions()
{
    Scene projected(ParseBlueprint(R"(
HStack(spacing:0) {
 Card(width:$lead,height:40)
 InteractionTarget(action:"shape",width:40,height:40,background:#FFFFFFFF) {
  Contour { Move(x:0,y:0) Line(x:40,y:0) Line(x:0,y:40) }
 }
}
)"),
                    Shape);
    assert(projected.SetBinding("lead", 10.0));
    assert(projected.SetViewport({8000, 80}));
    Build(projected);
    const auto id = FindAction(*projected.InputGeometry(), "shape");
    const RetainedState original(projected, id);
    std::string diagnostic;
    assert(!projected.Preflight({{"lead", 8190.0}}, &diagnostic));
    assert(!diagnostic.empty());
    original.Check(projected, id);
    assert(projected.ActionAt({15, 5}) == "shape");
    assert(!projected.ActionAt({45, 35}));
    assert(projected.HitTest({15, 5}, *original.snapshot)->node == id);

    Scene themed(ParseBlueprint(R"(
HStack(spacing:0) {
 Card(width:"@lead",height:40)
 InteractionTarget(action:"shape",width:40,height:40,background:"@surface") {
  Contour { Move(x:0,y:0) Line(x:40,y:0) Line(x:0,y:40) }
 }
}
)"),
                 Shape, {}, Theme(1));
    assert(themed.SetViewport({8000, 80}));
    Build(themed);
    const auto themed_id = FindAction(*themed.InputGeometry(), "shape");
    const auto unchanged = themed.InputGeometry();
    const auto contour = unchanged->Find(themed_id)->contour;
    assert(themed.ApplyTheme(Theme(2, 10, red)));
    const auto changed_color = Build(themed);
    assert(themed.ThemeGeneration() == 2 && FindFill(changed_color, red).contour == *contour);
    assert(themed.InputGeometry() == unchanged);
    assert(themed.InputGeometry()->version == unchanged->version);
    assert(themed.InputGeometry()->Find(themed_id)->contour == contour);

    const RetainedState retained(themed, themed_id);
    assert(!themed.ApplyTheme(Theme(3, 8190, red), &diagnostic));
    assert(!diagnostic.empty());
    retained.Check(themed, themed_id);
    assert(themed.ActionAt({15, 5}) == "shape");
    assert(!themed.ActionAt({45, 35}));
}

Blueprint Action(std::string action, contracts::Contour contour)
{
    Blueprint node;
    node.kind = Kind::InteractionTarget;
    node.properties = {{DslProperty::Action, std::move(action)},
                       {DslProperty::Width, 50.0},
                       {DslProperty::Height, 30.0},
                       {DslProperty::Background, white}};
    node.contour = std::move(contour);
    return node;
}

void CheckRegionProvenance()
{
    const contracts::Contour triangle{{{0, 0}, {50, 0}, {0, 30}}};
    const contracts::Contour rectangle{{{0, 0}, {50, 0}, {50, 30}, {0, 30}}};
    Blueprint root;
    root.kind = Kind::Column;
    Blueprint region;
    region.region = "first";
    region.properties = {{DslProperty::Width, 80.0}, {DslProperty::Height, 60.0}};
    region.children = {Action("placeholder", triangle)};
    root.children = {Action("retained", triangle), region};
    Scene scene(root, Shape);
    assert(scene.SetViewport({100, 100}));
    Build(scene);
    assert(scene.FocusNext() && scene.FocusedAction() == "retained");
    Build(scene);
    const auto retained = FindAction(*scene.InputGeometry(), "retained");
    const auto region_id = scene.RegionId("first");
    const auto root_id = scene.RootId();
    const RetainedState original(scene, retained);
    const auto state = scene.State(retained);
    const auto contour = original.snapshot->Find(retained)->contour;
    const RegionUpdate update{"first", Action("mounted", triangle)};
    std::string diagnostic;

    for (int alteration = 0; alteration < 3; ++alteration) {
        auto blueprint = scene.RegionBlueprint(std::span(&update, 1));
        if (alteration == 0) {
            blueprint.children.front().contour.reset();
        } else if (alteration == 1) {
            blueprint.children.front().contour = rectangle;
        } else {
            blueprint.children[1].children.front().contour = rectangle;
        }
        Scene candidate(std::move(blueprint), Shape);
        assert(!scene.MountRegions(std::span(&update, 1), {}, candidate, original.transaction,
                                   &diagnostic));
        assert(!diagnostic.empty() && candidate.RootId());
        original.Check(scene, retained);
        assert(scene.RootId() == root_id && scene.RegionId("first") == region_id);
        assert(scene.State(retained) == state && scene.FocusedAction() == "retained");
        assert(!scene.RegionMounted("first"));
        assert(scene.ActionAt({10, 10}) == "retained" && !scene.ActionAt({40, 25}));
    }

    Scene candidate(scene.RegionBlueprint(std::span(&update, 1)), Shape);
    assert(scene.MountRegions(std::span(&update, 1), {}, candidate, original.transaction));
    assert(!candidate.RootId() && scene.RegionMounted("first"));
    assert(scene.RootId() == root_id && scene.RegionId("first") == region_id);
    const auto list = Build(scene);
    const auto snapshot = scene.InputGeometry();
    const auto mounted = FindAction(*snapshot, "mounted");
    assert(snapshot->Find(retained)->contour == contour);
    assert(*snapshot->Find(mounted)->contour == Translate(triangle, 0, 30));
    assert(scene.ActionAt({5, 35}) == "mounted" && !scene.ActionAt({40, 55}));
    bool painted = false;
    for (const auto &command : list.commands) {
        if (const auto *fill = std::get_if<contracts::FillContour>(&command)) {
            painted = painted || fill->contour == *snapshot->Find(mounted)->contour;
        }
    }
    assert(painted);
}

void CheckPopupDismissal()
{
    Scene scene(ParseBlueprint(R"(
VStack(spacing:0) {
 Button("Open",action:"open",height:40)
 Button("Behind",action:"behind",height:100)
 Popup("open",width:200,height:100,background:#FFFFFFFF,cornerRadius:64) {
  Contour {
   Move(x:0,y:0) Line(x:200,y:0) Line(x:200,y:35)
   Line(x:100,y:35) Line(x:100,y:100) Line(x:0,y:100)
  }
  InteractionTarget(action:"done",width:200,height:100)
 }
}
)"),
                Shape);
    assert(scene.SetViewport({320, 240}));
    Build(scene);
    const auto anchor = FindAction(*scene.InputGeometry(), "open");
    assert(scene.OpenPopup(anchor));
    Build(scene);
    const auto old = scene.InputGeometry();
    scene.ApplyInputSnapshot(old);
    const auto done = FindAction(*old, "done");
    const auto bounds = scene.Bounds(done);
    const contracts::LogicalPoint outside{bounds.x + 150, bounds.y + 60};
    const auto *popup = old->Find(old->Find(done)->parent);
    assert(popup && popup->contour && !contracts::ContourContains(*popup->contour, outside));
    assert(!Button(scene, old, outside, true).activation && !scene.PopupToken());
    Build(scene);
    const auto closed = scene.InputGeometry();
    scene.ApplyInputSnapshot(closed);
    assert(!Button(scene, closed, outside, false).activation);
    const auto behind = Click(scene, closed, outside);
    assert(behind.activation && behind.activation->action == "behind");

    assert(scene.OpenPopup(anchor));
    Build(scene);
    const auto current = scene.InputGeometry();
    scene.ApplyInputSnapshot(current);
    const auto token = scene.PopupToken();
    const contracts::LogicalPoint inside{bounds.x + 10, bounds.y + 10};
    assert(!Click(scene, old, inside).activation && scene.PopupToken() == token);
    const auto command = Click(scene, current, inside);
    assert(command.activation && command.activation->action == "done");
    assert(!scene.PopupToken());
}
} // namespace

int main()
{
    CheckCanonicalGeometry();
    CheckAncestorIntersection();
    CheckScrollSnapshots();
    CheckScrollRegionRollback();
    CheckScrollEnvelopeRollback();
    CheckCollapsedScrolledContour();
    CheckTransactions();
    CheckRegionProvenance();
    CheckPopupDismissal();
}
