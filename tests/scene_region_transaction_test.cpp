#include "prism/runtime/scene_construction.hpp"
#include <cassert>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <variant>

using namespace prism;
using namespace prism::runtime;

namespace {
ShapedText Shape(std::string_view text, double size)
{
    ShapedText result;
    result.width = text.size() * 7;
    result.height = size;
    return result;
}

ShapedText RejectShape(std::string_view text, double size)
{
    if (text == "reject") {
        throw std::runtime_error("Rejected candidate shaping");
    }
    return Shape(text, size);
}

ShapedText StructuralShape(std::string_view text, double size)
{
    auto result = Shape(text, size);
    result.glyphs.resize(text.size() > 100000 ? text.size() : 1);
    if (text == "invalid-glyph") {
        result.glyphs.front().glyph_index = UINT16_MAX + 1U;
    } else if (text == "invalid-origin") {
        result.glyphs.front().origin.x = std::numeric_limits<double>::quiet_NaN();
    } else if (text == "invalid-metric") {
        result.width = std::numeric_limits<double>::quiet_NaN();
    }
    return result;
}

contracts::ThemeSnapshot Theme(std::uint64_t generation = 1)
{
    contracts::ThemeSnapshot value;
    value.id = "fixture";
    value.name = "Fixture";
    value.generation = generation;
    value.colors = {{"surface", {20, 30, 40, 255}}};
    return value;
}

Blueprint Box(double width, double height)
{
    Blueprint value;
    value.properties = {{DslProperty::Width, width}, {DslProperty::Height, height}};
    return value;
}

Blueprint Action(std::string name)
{
    auto value = Box(50, 30);
    value.properties.push_back({DslProperty::Action, std::move(name)});
    value.properties.push_back({DslProperty::Background, contracts::Color{70, 80, 90, 255}});
    return value;
}

Blueprint Slot(std::string name, std::string placeholder)
{
    auto value = Box(100, 60);
    value.region = std::move(name);
    value.children.push_back(Action(std::move(placeholder)));
    return value;
}

Blueprint Layout()
{
    Blueprint value;
    value.kind = Kind::Column;
    auto retained = Action("retained");
    retained.bindings = {{"retained_color", DslProperty::Background}};
    value.children = {std::move(retained), Slot("first", "old-first"),
                      Slot("second", "old-second")};
    return value;
}

std::unique_ptr<Scene> Construct(Blueprint blueprint,
                                 std::optional<contracts::ThemeSnapshot> theme = {})
{
    SceneConstruction builder(std::move(blueprint), Shape, {}, std::move(theme));
    const auto deadline = std::chrono::steady_clock::time_point::max();
    while (!builder.Advance(2, deadline)) {
    }
    return builder.TakeScene();
}

contracts::DisplayList Build(Scene &scene)
{
    auto list = scene.Build({1});
    assert(list);
    scene.AcknowledgeComposite();
    return *list;
}

bool HasColor(const contracts::DisplayList &list, contracts::Color color)
{
    for (const auto &command : list.commands) {
        if (const auto *fill = std::get_if<contracts::FillRect>(&command)) {
            if (fill->color == color) {
                return true;
            }
        }
    }
    return false;
}

void CheckConstruction()
{
    auto source = Layout();
    SceneConstruction builder(source, Shape);
    assert(!builder.Ready() && builder.ConstructedNodes() == 0);
    assert(!builder.Advance(128, std::chrono::steady_clock::time_point::min()));
    assert(builder.ConstructedNodes() == 0);
    assert(!builder.Advance(1, std::chrono::steady_clock::time_point::max()));
    assert(builder.ConstructedNodes() == 1);
    while (!builder.Advance(2, std::chrono::steady_clock::time_point::max())) {
    }
    assert(builder.ConstructedNodes() == 6);
    auto built = builder.TakeScene();
    Scene direct(source, Shape);
    assert(built->SetViewport({300, 200}) && direct.SetViewport({300, 200}));
    assert(Build(*built).commands == Build(direct).commands);
    bool consumed = false;
    try {
        (void)builder.TakeScene();
    } catch (const std::logic_error &) {
        consumed = true;
    }
    assert(consumed);
}

void CheckStableMount()
{
    auto scene = Construct(Layout());
    assert(scene->SetViewport({300, 200}));
    Build(*scene);
    const auto root = scene->RootId();
    const auto first = scene->RegionId("first");
    const auto second = scene->RegionId("second");
    const contracts::NodeId retained{1, 1};
    const contracts::NodeId removed{3, 1};
    assert(scene->FocusNext() && scene->FocusedAction() == "retained");
    assert(scene->SetPointer({10, 10}));
    const auto revision = scene->TransactionRevision();
    auto content = Action("new-first");
    content.bindings.push_back({"new_width", DslProperty::Width});
    const RegionUpdate update{"first", content};
    auto candidate = Construct(scene->RegionBlueprint(std::span(&update, 1)));
    assert(scene->TransactionRevision() == revision); // focus/pointer do not stale a candidate
    const contracts::Color green{10, 180, 30, 255};
    assert(scene->MountRegions(
        std::span(&update, 1),
        {{"retained_color", green}, {"new_width", 23.0}, {"not_mounted_yet", true}}, *candidate,
        revision));
    assert(scene->RootId() == root && scene->RegionId("first") == first);
    assert(scene->RegionId("second") == second && scene->RegionMounted("first"));
    assert(!scene->RegionMounted("second"));
    assert(scene->FocusedAction() == "retained");
    assert(!scene->SetPointer({10, 10}));
    assert(!scene->IsVisible(removed) && scene->Bounds(removed) == contracts::LogicalRect{});
    assert(scene->IsVisible(retained));
    assert(!candidate->RootId() && !candidate->SetBinding("new_width", 99.0));
    auto list = Build(*scene);
    assert(HasColor(list, green));
    assert(scene->Bounds({6, 1}).width == 23);
    assert(scene->ActionAt({10, 40}) == "new-first");
    assert(scene->SetBinding("new_width", 35.0));
    Build(*scene);
    assert(scene->Bounds({6, 1}).width == 35);

    // A second mount produces a new monotonic ID and keeps the first region intact.
    const RegionUpdate other{"second", Action("new-second")};
    assert(scene->MountRegions(std::span(&other, 1), {}));
    Build(*scene);
    assert(scene->Bounds({6, 1}).width == 35 && scene->IsVisible({7, 1}));
    assert(!scene->IsVisible({5, 1}));
    assert(scene->FocusNext() && scene->FocusedAction() == "new-first");
    assert(scene->FocusNext() && scene->FocusedAction() == "new-second");
    auto changed = Theme(2);
    assert(scene->ApplyTheme(changed)); // theme rebuild works with sparse IDs/bindings
    assert(scene->SetBinding("new_width", 39.0));
    Build(*scene);
    assert(scene->Bounds({6, 1}).width == 39 && scene->RegionMounted("first"));
}

void CheckRollback()
{
    Scene scene(Layout(), Shape);
    assert(scene.SetViewport({300, 200}));
    const auto original = Build(scene);
    assert(scene.FocusNext());
    assert(scene.FocusNext() && scene.FocusedAction() == "old-first");
    Build(scene);
    const auto stats = scene.GetRenderStats();
    const auto revision = scene.TransactionRevision();
    const auto pixels = scene.PixelsRevision();
    const auto bounds = scene.Bounds({3, 1});
    std::string diagnostic;
    const RegionUpdate good{"first", Action("new")};
    auto candidate = Construct(scene.RegionBlueprint(std::span(&good, 1)));
    assert(!scene.MountRegions(std::span(&good, 1), {{"retained_color", std::string("bad")}},
                               *candidate, revision, &diagnostic));
    assert(!diagnostic.empty());
    assert(scene.GetRenderStats() == stats && scene.TransactionRevision() == revision);
    assert(scene.PixelsRevision() == pixels && scene.Bounds({3, 1}) == bounds);
    assert(scene.FocusedAction() == "old-first" && !scene.RegionMounted("first"));
    const RegionUpdate unknown{"missing", Action("new")};
    assert(!scene.MountRegions(std::span(&unknown, 1), {}, &diagnostic));
    const RegionUpdate duplicate[] = {good, good};
    assert(!scene.MountRegions(duplicate, {}, &diagnostic));
    assert(scene.GetRenderStats() == stats && scene.TransactionRevision() == revision);
    assert(scene.MountRegions(std::span(&good, 1), {}, *candidate, revision));
    assert(!scene.FocusedAction()); // only removed placeholder focus is cleared
    Build(scene);
    assert(!scene.MountRegions(std::span(&good, 1), {}, &diagnostic));
    assert(scene.RegionMounted("first"));
    (void)original;
}

void CheckLatestStateAndTheme()
{
    auto theme = Theme();
    Scene scene(Layout(), Shape, {}, theme);
    assert(scene.SetViewport({300, 200}));
    Build(scene);
    auto colored = Action("colored");
    colored.theme_refs.push_back({"surface", DslProperty::Background});
    const RegionUpdate update{"first", colored};
    auto candidate = Construct(scene.RegionBlueprint(std::span(&update, 1)), theme);
    const auto revision = scene.TransactionRevision();
    assert(scene.SetProperty({1, 1}, DslProperty::Width, 75.0));
    assert(!scene.MountRegions(std::span(&update, 1), {}, *candidate, revision));
    theme.generation = 2;
    theme.colors.front().value = {150, 20, 100, 255};
    assert(scene.ApplyTheme(theme));
    candidate = Construct(scene.RegionBlueprint(std::span(&update, 1)), theme);
    assert(scene.MountRegions(std::span(&update, 1), {}, *candidate, scene.TransactionRevision()));
    auto list = Build(scene);
    assert(HasColor(list, theme.colors.front().value));
    assert(scene.Bounds({1, 1}).width == 75); // unbound property survives mount and theme

    const auto stats = scene.GetRenderStats();
    const auto before = scene.PixelsRevision();
    assert(!scene.Preflight({{"retained_color", 3.0}}));
    assert(scene.GetRenderStats() == stats && scene.PixelsRevision() == before);
    const contracts::Color projected{1, 2, 3, 255};
    assert(scene.Preflight({{"retained_color", projected}, {"future_key", false}}));
    assert(HasColor(Build(scene), projected));
}

void CheckBatchOrderAndDetached()
{
    Scene scene(Layout(), Shape);
    assert(scene.SetViewport({300, 200}));
    Build(scene);
    auto first = Action("first-action");
    auto second = Action("second-action");
    const contracts::Color red{210, 10, 10, 255};
    const contracts::Color blue{10, 10, 210, 255};
    first.properties.push_back({DslProperty::Background, red});
    second.properties.push_back({DslProperty::Background, blue});
    // Completion order is intentionally the reverse of authored slot order.
    const RegionUpdate updates[] = {{"second", second}, {"first", first}};
    auto candidate = Construct(scene.RegionBlueprint(updates));
    assert(candidate->SetViewport({300, 200}));
    assert(candidate->PrepareDetached({}));
    assert(Has(candidate->PendingDirty(), Dirty::Paint));
    assert(candidate->Build({1})); // validation must not consume first pixel list
    assert(scene.MountRegions(updates, {}, *candidate, scene.TransactionRevision()));
    const auto list = Build(scene);
    std::vector<contracts::Color> colors;
    for (const auto &command : list.commands) {
        if (const auto *fill = std::get_if<contracts::FillRect>(&command)) {
            if (fill->color == red || fill->color == blue) {
                colors.push_back(fill->color);
            }
        }
    }
    assert((colors == std::vector<contracts::Color>{red, blue}));
    assert(scene.RegionMounted("first") && scene.RegionMounted("second"));

    Scene rollback(Layout(), Shape);
    assert(rollback.SetViewport({300, 200}));
    Build(rollback);
    const auto revision = rollback.TransactionRevision();
    auto invalid = second;
    invalid.theme_refs.push_back({"missing", DslProperty::Background});
    const RegionUpdate bad[] = {{"first", first}, {"second", invalid}};
    assert(!rollback.MountRegions(bad, {}));
    assert(!rollback.RegionMounted("first") && !rollback.RegionMounted("second"));
    assert(rollback.TransactionRevision() == revision);
}

void CheckShapingFailure()
{
    Scene scene(Layout(), RejectShape);
    assert(scene.SetViewport({300, 200}));
    Build(scene);
    const auto revision = scene.TransactionRevision();
    const auto stats = scene.GetRenderStats();
    Blueprint text;
    text.kind = Kind::Text;
    text.properties.push_back({DslProperty::Text, std::string("reject")});
    const RegionUpdate update{"first", std::move(text)};
    std::string diagnostic;
    assert(!scene.MountRegions(std::span(&update, 1), {}, &diagnostic));
    assert(diagnostic == "Rejected candidate shaping");
    assert(scene.TransactionRevision() == revision && scene.GetRenderStats() == stats);
    assert(!scene.RegionMounted("first") && scene.ActionAt({10, 40}) == "old-first");
}

void CheckCandidateProvenance()
{
    auto theme = Theme();
    theme.colors.push_back({"alias", theme.colors.front().value});
    auto layout = Layout();
    layout.children.front().theme_refs.push_back({"surface", DslProperty::Background});
    Scene scene(layout, Shape, {}, theme);
    assert(scene.SetViewport({300, 200}));
    assert(scene.SetProperty({1, 1}, DslProperty::Width, 75.0));
    Build(scene);
    assert(scene.FocusNext() && scene.FocusedAction() == "retained");
    Build(scene);
    const auto revision = scene.TransactionRevision();
    const auto stats = scene.GetRenderStats();
    const auto pixels = scene.PixelsRevision();
    const RegionUpdate update{"first", Action("new")};
    std::string diagnostic;

    Scene foreign(layout, Shape, {}, theme);
    auto candidate = Construct(foreign.RegionBlueprint(std::span(&update, 1)), theme);
    assert(!scene.MountRegions(std::span(&update, 1), {}, *candidate, revision, &diagnostic));
    assert(!diagnostic.empty() && candidate->RootId());
    assert(scene.Bounds({1, 1}).width == 75 && scene.FocusedAction() == "retained");

    candidate = Construct(scene.RegionBlueprint(std::span(&update, 1)), theme);
    assert(candidate->SetProperty({1, 1}, DslProperty::Action, std::string("forged")));
    assert(!scene.MountRegions(std::span(&update, 1), {}, *candidate, revision));
    assert(scene.ActionAt({10, 10}) == "retained");

    auto modified = scene.RegionBlueprint(std::span(&update, 1));
    modified.children.front().theme_refs.front().name = "alias";
    candidate = Construct(std::move(modified), theme);
    // The alias currently resolves to the same color; provenance still differs.
    assert(!scene.MountRegions(std::span(&update, 1), {}, *candidate, revision));
    modified = scene.RegionBlueprint(std::span(&update, 1));
    modified.children.front().theme_refs.clear();
    modified.children.front().properties.push_back(
        {DslProperty::Background, theme.colors.front().value});
    candidate = Construct(std::move(modified), theme);
    assert(!scene.MountRegions(std::span(&update, 1), {}, *candidate, revision));
    assert(scene.GetRenderStats() == stats && scene.PixelsRevision() == pixels);
    assert(scene.TransactionRevision() == revision && !scene.RegionMounted("first"));

    candidate = Construct(scene.RegionBlueprint(std::span(&update, 1)), theme);
    assert(candidate->SetViewport({300, 200}));
    const contracts::Color projected{50, 180, 70, 255};
    assert(candidate->PrepareDetached({{"retained_color", projected}}));
    assert(scene.MountRegions(std::span(&update, 1), {{"retained_color", projected}}, *candidate,
                              revision));
    assert(scene.Bounds({1, 1}).width == 75 && scene.FocusedAction() == "retained");
    assert(HasColor(Build(scene), projected));
}

void CheckStructuralRollback()
{
    Scene scene(Layout(), StructuralShape, {7});
    assert(scene.SetViewport({300, 200}));
    Build(scene);
    const auto revision = scene.TransactionRevision();
    const auto stats = scene.GetRenderStats();
    const auto pixels = scene.PixelsRevision();
    const std::string cases[] = {std::string(100001, 'i'), "invalid-glyph", "invalid-origin",
                                 "invalid-metric"};
    for (const auto &text : cases) {
        Blueprint content;
        content.kind = Kind::Text;
        content.properties.push_back({DslProperty::Text, text});
        const RegionUpdate update{"first", std::move(content)};
        std::string diagnostic;
        assert(!scene.MountRegions(std::span(&update, 1), {}, &diagnostic));
        assert(!diagnostic.empty());
        assert(scene.TransactionRevision() == revision && scene.GetRenderStats() == stats);
        assert(scene.PixelsRevision() == pixels && !scene.RegionMounted("first"));
        assert(scene.ActionAt({10, 40}) == "old-first");
    }
    // The backend's exact glyph count boundary is still accepted.
    Blueprint boundary;
    boundary.kind = Kind::Text;
    boundary.properties.push_back({DslProperty::Text, std::string(100000, 'i')});
    const RegionUpdate valid{"first", std::move(boundary)};
    assert(scene.MountRegions(std::span(&valid, 1), {}));
    assert(scene.RegionMounted("first"));
}

void CheckCombinedLimits()
{
    Scene scene(Layout(), Shape);
    assert(scene.SetViewport({300, 200}));
    Build(scene);
    const auto revision = scene.TransactionRevision();
    auto large = Box(50, 30);
    large.children.resize(8192, Box(1, 1));
    const RegionUpdate too_many{"first", std::move(large)};
    assert(!scene.MountRegions(std::span(&too_many, 1), {}));
    auto deep = Box(1, 1);
    for (int i = 0; i < 64; ++i) {
        auto parent = Box(1, 1);
        parent.children.push_back(std::move(deep));
        deep = std::move(parent);
    }
    const RegionUpdate too_deep{"first", std::move(deep)};
    assert(!scene.MountRegions(std::span(&too_deep, 1), {}));
    auto effects = Box(50, 30);
    for (int i = 0; i < 9; ++i) {
        auto effect = Box(10, 10);
        effect.properties.push_back({DslProperty::BackdropBlur, 2.0});
        effects.children.push_back(std::move(effect));
    }
    const RegionUpdate too_effective{"first", std::move(effects)};
    assert(!scene.MountRegions(std::span(&too_effective, 1), {}));
    assert(scene.TransactionRevision() == revision && !scene.RegionMounted("first"));
    auto invalid = Action("invalid");
    invalid.properties.push_back({DslProperty::Width, -1.0});
    const RegionUpdate bad_property{"first", std::move(invalid)};
    assert(!scene.MountRegions(std::span(&bad_property, 1), {}));
    assert(scene.IsVisible({3, 1}));
}
} // namespace

int main()
{
    CheckConstruction();
    CheckStableMount();
    CheckRollback();
    CheckLatestStateAndTheme();
    CheckBatchOrderAndDetached();
    CheckShapingFailure();
    CheckCandidateProvenance();
    CheckStructuralRollback();
    CheckCombinedLimits();
}
