#include "prism/contracts/display_list.hpp"
#include "prism/runtime/popup_surface.hpp"
#include "prism/runtime/scene.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::Color panel_color{32, 42, 65, 255};
constexpr contracts::InputSource mouse{1, 7, 1};
constexpr contracts::InputSource keyboard{1, 8, 1};

struct ShapeCounter {
    std::size_t *calls{};

    ShapedText operator()(std::string_view text, double font) const
    {
        if (calls) {
            ++*calls;
        }
        return {{}, text.size() * font / 2, font};
    }
};

Blueprint Box(double width = 0, double height = 0)
{
    Blueprint result;
    result.properties = {{DslProperty::Width, width}, {DslProperty::Height, height}};
    return result;
}

Blueprint Target(std::string action, double width = 0, double height = 28)
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
    result.properties.push_back({DslProperty::Value, 0.5});
    for (std::string role : {"track", "fill", "thumb"}) {
        auto part = Box(role == "thumb" ? 12 : 0, role == "thumb" ? 12 : 4);
        part.kind = Kind::Visual;
        part.properties.push_back({DslProperty::SliderPart, role});
        part.properties.push_back({DslProperty::Background, contracts::Color{100, 140, 200, 255}});
        result.children.push_back(std::move(part));
    }
    return result;
}

Blueprint Layout(bool scroll = false)
{
    Blueprint root;
    Blueprint column;
    column.kind = Kind::Column;
    column.children.push_back(Box(0, 40));
    auto row = Box(0, 32);
    row.kind = Kind::Row;
    row.children = {Box(140, 32), Target("sound", 64, 32)};
    column.children.push_back(std::move(row));
    root.children.push_back(std::move(column));

    auto popup = Box(260, 240);
    popup.kind = Kind::Popup;
    popup.contour_recipe = AttachedPanelRecipe{12.0, 28.0, 16.0, ContourFallback::Detached};
    popup.properties.push_back({DslProperty::PopupFor, std::string("sound")});
    popup.properties.push_back({DslProperty::Padding, 12.0});
    popup.properties.push_back({DslProperty::Background, panel_color});
    popup.properties.push_back({DslProperty::ShadowBlur, 6.0});
    popup.properties.push_back({DslProperty::ShadowColor, contracts::Color{0, 0, 0, 150}});
    Blueprint content;
    content.kind = Kind::Column;
    content.properties.push_back({DslProperty::Spacing, 8.0});
    content.children.push_back(Slider());
    auto checkbox = Box(0, 26);
    checkbox.kind = Kind::Checkbox;
    checkbox.properties.push_back({DslProperty::Action, std::string("muted")});
    content.children.push_back(std::move(checkbox));
    auto label = Box(0, 20);
    label.kind = Kind::Text;
    label.properties.push_back({DslProperty::Text, std::string("Volume")});
    content.children.push_back(std::move(label));
    if (scroll) {
        auto view = Box();
        view.kind = Kind::ScrollView;
        auto flow = Box(0, 420);
        flow.kind = Kind::Column;
        flow.children.push_back(Target("scrolled"));
        view.children.push_back(std::move(flow));
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

contracts::LogicalPoint Center(contracts::LogicalRect bounds)
{
    return {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2};
}

bool HasPanelColor(const contracts::DisplayList &list)
{
    for (const auto &command : list.commands) {
        const auto *contour = std::get_if<contracts::FillContour>(&command);
        if (contour && contour->color == panel_color) {
            return true;
        }
    }
    return false;
}

struct Fixture {
    std::size_t shape_calls{};
    Scene scene;
    contracts::NodeId anchor;

    explicit Fixture(Blueprint blueprint = Layout())
        : scene(std::move(blueprint), ShapeCounter{&shape_calls})
    {
        assert(scene.SetViewport({640, 480}));
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

    PopupSurfaceConfigure Configure(const PopupSurfaceRequest &request, double width = 180,
                                    double height = 186)
    {
        return {request.parent_configure_generation,
                9,
                {std::floor(request.anchor.x + request.anchor.width / 2 - width / 2),
                 std::ceil(request.anchor.y + request.anchor.height + request.gap), width, height}};
    }

    PopupSurfacePlan Plan(double width = 180, double height = 186)
    {
        const auto request = Request();
        const auto plan = scene.PreparePopupSurface(request, Configure(request, width, height));
        assert(plan);
        return *plan;
    }
};

PopupSurfaceIdentity Identity(std::uint64_t sequence = 1, std::uint64_t target = 51)
{
    return {3, target, target, 9, sequence};
}

InteractionResult Button(Scene &scene, const PopupSurfacePlan &plan,
                         const PopupSurfaceIdentity &identity, contracts::LogicalPoint point,
                         contracts::ButtonState state)
{
    return scene.HandlePopupSurfaceInput(
        contracts::PointerButtonEvent{
            {17}, point, contracts::PointerButton::Primary, state, 0, 1, mouse, 42},
        identity, plan.input_snapshot);
}

InteractionResult Click(Scene &scene, const PopupSurfacePlan &plan,
                        const PopupSurfaceIdentity &identity, std::string_view action)
{
    const auto point =
        Center(plan.input_snapshot->Find(Action(*plan.input_snapshot, action))->bounds);
    Button(scene, plan, identity, point, contracts::ButtonState::Pressed);
    return Button(scene, plan, identity, point, contracts::ButtonState::Released);
}

InteractionResult Key(Scene &scene, const PopupSurfacePlan &plan,
                      const PopupSurfaceIdentity &identity, std::uint32_t key,
                      contracts::ButtonState state)
{
    return scene.HandlePopupSurfaceInput(
        contracts::KeyEvent{{17}, key, state, false, 1, keyboard, {}}, identity,
        plan.input_snapshot);
}

void ProvenanceAndRootHandoff()
{
    Fixture f;
    const auto plan = f.Plan();
    const auto identity = Identity();
    const auto old_root = f.scene.InputGeometry();
    const auto root_bounds = f.scene.Bounds(f.scene.RootId());
    const auto popup_bounds = f.scene.Bounds(plan.request.active_node);
    const auto before_pixels = f.scene.PixelsRevision();

    assert(!Click(f.scene, plan, identity, "muted").control_edit);
    assert(!f.scene.HitTest(Center(plan.body_geometry), *plan.input_snapshot));
    assert(!f.scene.ApplyInputSnapshot(plan.input_snapshot));
    f.scene.HandleInput(contracts::FocusEvent{{17}, false, keyboard, false}, plan.input_snapshot);
    assert(f.scene.PopupToken() == plan.request.popup_token);
    Fixture other_scene;
    assert(!other_scene.scene.AdoptPopupSurface(plan, identity));
    for (int invalid = 0; invalid != 6; ++invalid) {
        auto identity_copy = identity;
        if (invalid == 0) {
            identity_copy.worker = 0;
        } else if (invalid == 1) {
            identity_copy.target = 0;
        } else if (invalid == 2) {
            ++identity_copy.lifetime;
        } else if (invalid == 3) {
            identity_copy.configure_generation = 0;
        } else if (invalid == 4) {
            ++identity_copy.configure_generation;
        } else {
            identity_copy.submission_sequence = 0;
        }
        assert(!f.scene.AdoptPopupSurface(plan, identity_copy));
    }
    auto forged = plan;
    ++forged.window_geometry.width;
    assert(!f.scene.AdoptPopupSurface(forged, identity));
    forged = plan;
    forged.input_snapshot = std::make_shared<const InputSnapshot>(*plan.input_snapshot);
    assert(!f.scene.AdoptPopupSurface(forged, identity));
    forged = plan;
    forged.prepared.reset();
    assert(!f.scene.AdoptPopupSurface(forged, identity));

    assert(f.scene.AdoptPopupSurface(plan, identity));
    assert(f.scene.HasPopupSurfaceAdoption());
    assert(f.scene.PopupSurfaceAdoptionIdentity() == identity);
    assert(f.scene.PixelsRevision() > before_pixels);
    assert(f.scene.Bounds(f.scene.RootId()) == root_bounds);
    assert(f.scene.Bounds(plan.request.active_node) == popup_bounds);
    const auto detached_root = f.scene.InputGeometry();
    assert(detached_root->version > old_root->version);
    assert(!detached_root->Find(plan.request.active_node));
    assert(!detached_root->Find(Action(*plan.input_snapshot, "muted")));
    assert(detached_root->popup_token == plan.request.popup_token);
    assert(!f.scene.AdoptPopupSurface(plan, identity));

    const auto root_list = f.scene.Build({17});
    assert(root_list && !HasPanelColor(*root_list));
    const auto request = f.Request();
    const auto child = f.scene.PreparePopupSurface(request, f.Configure(request));
    assert(child && HasPanelColor(*child->display_list));

    assert(!f.scene.RevokePopupSurface(Identity(0)));
    assert(f.scene.RevokePopupSurface(identity));
    assert(!f.scene.HasPopupSurfaceAdoption());
    const auto restored_root = f.scene.InputGeometry();
    assert(restored_root->version > detached_root->version);
    assert(restored_root->Find(plan.request.active_node));
    const auto fallback = f.scene.Build({17});
    assert(fallback && HasPanelColor(*fallback));
    assert(!f.scene.RevokePopupSurface(identity));
}

void InputUsesConfiguredGeometry()
{
    Fixture f;
    auto plan = f.Plan();
    auto identity = Identity();
    const auto muted = Action(*plan.input_snapshot, "muted");
    const auto root_muted = f.scene.InputGeometry()->Find(muted)->bounds;
    const auto local_muted = plan.input_snapshot->Find(muted)->bounds;
    assert(local_muted.width != root_muted.width);
    assert(f.scene.AdoptPopupSurface(plan, identity));
    const auto point = Center(local_muted);
    Button(f.scene, plan, identity, point, contracts::ButtonState::Pressed);
    assert(f.scene.State(muted).captured);
    assert(f.scene.Build({17}));
    f.scene.ApplyInputSnapshot(f.scene.InputGeometry());
    assert(f.scene.State(muted).captured);
    auto wrong = identity;
    ++wrong.submission_sequence;
    assert(!Button(f.scene, plan, wrong, point, contracts::ButtonState::Released).control_edit);
    assert(f.scene.State(muted).captured);
    const auto edit = Button(f.scene, plan, identity, point, contracts::ButtonState::Released);
    assert(edit.control_edit && edit.control_edit->action == "muted");
    assert(std::get<bool>(edit.control_edit->event.value));
    assert(!f.scene.State(muted).captured);
    Key(f.scene, plan, identity, 0x2c, contracts::ButtonState::Pressed);
    const auto keyboard_edit = Key(f.scene, plan, identity, 0x2c, contracts::ButtonState::Released);
    assert(keyboard_edit.control_edit && keyboard_edit.control_edit->action == "muted");

    assert(f.scene.Build({17}));
    plan = f.Plan();
    ++identity.submission_sequence;
    assert(f.scene.AdoptPopupSurface(plan, identity));
    const auto volume = Action(*plan.input_snapshot, "volume");
    const auto track = plan.input_snapshot->Find(volume)->slider_track;
    const contracts::LogicalPoint proposal{track.x + track.width * 0.75, track.y};
    Button(f.scene, plan, identity, proposal, contracts::ButtonState::Pressed);
    auto events = f.scene.TakeControlEvents();
    assert(events.size() == 1 && events.front().event.phase == ValuePhase::Preview);
    assert(std::abs(std::get<double>(events.front().event.value) - 0.75) < 1e-9);
    assert(f.scene.Build({17}));
    f.scene.ApplyInputSnapshot(f.scene.InputGeometry());
    Button(f.scene, plan, identity, proposal, contracts::ButtonState::Released);
    events = f.scene.TakeControlEvents();
    assert(events.size() == 1 && events.front().event.phase == ValuePhase::Commit);
    assert(std::abs(std::get<double>(events.front().event.value) - 0.75) < 1e-9);

    assert(f.scene.Build({17}));
    plan = f.Plan();
    ++identity.submission_sequence;
    assert(f.scene.AdoptPopupSurface(plan, identity));
    Button(f.scene, plan, identity, point, contracts::ButtonState::Pressed);
    assert(f.scene.SetEnabled(muted, false));
    assert(!Button(f.scene, plan, identity, point, contracts::ButtonState::Released).control_edit);
    assert(f.scene.RevokePopupSurface(identity));
    assert(!Button(f.scene, plan, identity, point, contracts::ButtonState::Pressed).control_edit);
}

void PaintingReusesLayout()
{
    Fixture f;
    const auto initial = f.Plan();
    assert(f.scene.AdoptPopupSurface(initial, Identity()));
    assert(f.scene.Build({17}));
    const auto before = f.scene.GetPopupSurfacePreparationStats();
    const auto root_stats = f.scene.GetRenderStats();
    const auto shapes = f.shape_calls;
    for (unsigned i = 0; i != 8; ++i) {
        assert(f.scene.SetBackground(initial.request.active_node,
                                     {32, static_cast<std::uint8_t>(60 + i), 65, 255}));
        assert(f.scene.Build({17}));
        const auto next = f.Plan();
        assert(next.body_geometry == initial.body_geometry);
        assert(
            next.input_snapshot->Find(Action(*next.input_snapshot, "volume"))->slider_track ==
            initial.input_snapshot->Find(Action(*initial.input_snapshot, "volume"))->slider_track);
    }
    const auto after = f.scene.GetPopupSurfacePreparationStats();
    assert(after.preparations == before.preparations + 8);
    assert(after.layouts == before.layouts && after.layout_reuses == before.layout_reuses + 8);
    assert(f.scene.GetRenderStats().layouts == root_stats.layouts);
    assert(f.shape_calls == shapes);

    const auto volume = Action(*initial.input_snapshot, "volume");
    assert(f.scene.SetProperty(volume, DslProperty::Height, 42.0));
    assert(f.scene.Build({17}));
    const auto relaid = f.Plan();
    assert(f.scene.GetPopupSurfacePreparationStats().layouts == after.layouts + 1);
    assert(f.shape_calls > shapes);
    assert(relaid.input_snapshot->Find(volume)->bounds.height == 42);
}

void ScrollUsesChildMetrics()
{
    Fixture f(Layout(true));
    auto plan = f.Plan(180, 180);
    contracts::NodeId scroll;
    for (const auto &node : plan.input_snapshot->nodes) {
        if (node.id && node.clip && node.id != plan.request.active_node && node.action.empty() &&
            node.children.size() == 1) {
            scroll = node.id;
        }
    }
    assert(scroll);
    const auto root_scroll_bounds = f.scene.Bounds(scroll);
    const auto root_metrics = f.scene.ScrollInfo(scroll);
    auto identity = Identity();
    assert(f.scene.AdoptPopupSurface(plan, identity));
    const auto local_metrics = f.scene.ScrollInfo(scroll);
    assert(local_metrics && root_metrics &&
           local_metrics->viewport_height != root_metrics->viewport_height);
    assert(local_metrics->viewport_height == plan.input_snapshot->Find(scroll)->bounds.height);
    const auto local_bounds = plan.input_snapshot->Find(scroll)->bounds;
    const auto point = Center(local_bounds);
    const auto result = f.scene.HandlePopupSurfaceInput(
        contracts::PointerScrollEvent{{17}, point, 0, 20, 1, mouse}, identity, plan.input_snapshot);
    assert(result.changed && f.scene.ScrollInfo(scroll)->offset == 60);
    const auto burst = f.scene.HandlePopupSurfaceInput(
        contracts::PointerScrollEvent{{17}, point, 0, 20, 2, mouse}, identity, plan.input_snapshot);
    assert(burst.changed && f.scene.ScrollInfo(scroll)->offset == 120);
    ++identity.submission_sequence;
    assert(f.scene.AdoptPopupSurface(
        plan, identity)); // A trailing offset-zero commit cannot rewind input.
    assert(f.scene.ScrollInfo(scroll)->offset == 120);
    assert(f.scene.Bounds(scroll) == root_scroll_bounds);
    const auto stats = f.scene.GetPopupSurfacePreparationStats();
    assert(f.scene.Build({17}));
    const auto old_plan = plan;
    plan = f.Plan(180, 180);
    assert(plan.input_snapshot->Find(scroll)->scroll_offset == 120);
    assert(f.scene.GetPopupSurfacePreparationStats().layouts == stats.layouts);
    assert(f.scene.GetPopupSurfacePreparationStats().layout_reuses == stats.layout_reuses + 1);
    ++identity.submission_sequence;
    assert(f.scene.AdoptPopupSurface(plan, identity));
    const auto scrolled = Action(*plan.input_snapshot, "scrolled");
    const auto old_position = Center(old_plan.input_snapshot->Find(scrolled)->bounds);
    const auto stale = f.scene.HandlePopupSurfaceInput(
        contracts::PointerButtonEvent{{17},
                                      old_position,
                                      contracts::PointerButton::Primary,
                                      contracts::ButtonState::Pressed,
                                      0,
                                      1,
                                      mouse},
        Identity(), old_plan.input_snapshot);
    assert(!stale.activation && !stale.control_edit);
    assert(!f.scene.State(scrolled).captured);
    assert(f.scene.ScrollTo(scroll, 10000));
    assert(f.scene.ScrollInfo(scroll)->offset == local_metrics->maximum);
}

void FocusAndCloseScopes()
{
    Fixture f;
    auto plan = f.Plan();
    auto identity = Identity();
    f.scene.HandleInput(contracts::FocusEvent{{17}, false, keyboard, true},
                        f.scene.InputGeometry());
    assert(f.scene.PopupToken() == plan.request.popup_token);
    assert(f.scene.AdoptPopupSurface(plan, identity));
    const auto muted = Action(*plan.input_snapshot, "muted");
    const auto point = Center(plan.input_snapshot->Find(muted)->bounds);
    Button(f.scene, plan, identity, point, contracts::ButtonState::Pressed);
    f.scene.HandleInput(contracts::FocusEvent{{17}, false, keyboard, true},
                        f.scene.InputGeometry());
    assert(f.scene.HasPopupSurfaceAdoption() && f.scene.State(muted).captured);
    // Keyboard focus can transfer back from the child to its owner while a
    // pointer button remains held. Both surfaces belong to the same Scene.
    f.scene.HandlePopupSurfaceInput(contracts::FocusEvent{{17}, false, keyboard, true}, identity,
                                    plan.input_snapshot);
    assert(f.scene.HasPopupSurfaceAdoption() && f.scene.State(muted).captured);
    const auto released = Button(f.scene, plan, identity, point, contracts::ButtonState::Released);
    assert(released.control_edit);

    assert(f.scene.Build({17}));
    plan = f.Plan();
    ++identity.submission_sequence;
    assert(f.scene.AdoptPopupSurface(plan, identity));
    const auto pixels = f.scene.PixelsRevision();
    assert(f.scene.AdoptPopupSurface(plan, Identity(identity.submission_sequence + 1)));
    assert(f.scene.PixelsRevision() == pixels); // Metadata adoption is not an endless root repaint.
    ++identity.submission_sequence;
    f.scene.HandlePopupSurfaceInput(contracts::FocusEvent{{17}, false, keyboard, false}, identity,
                                    plan.input_snapshot);
    assert(!f.scene.PopupToken() && !f.scene.HasPopupSurfaceAdoption());
    assert(!Click(f.scene, plan, identity, "muted").control_edit);
    assert(f.scene.Build({17}));
    assert(f.scene.OpenPopup(f.anchor));
    assert(f.scene.Build({17}));
    assert(f.scene.PopupToken() != plan.request.popup_token);
    assert(!f.scene.AdoptPopupSurface(plan, Identity(100)));
}

void BackdropCapability()
{
    Fixture f;
    auto request = f.Request();
    assert(!request.requires_backdrop);
    const auto muted = Action(*f.scene.InputGeometry(), "muted");
    const auto pixels = f.scene.PixelsRevision();
    const auto stats = f.scene.GetRenderStats();

    assert(f.scene.SetProperty(muted, DslProperty::BackdropBlur, 8.0));
    assert(Has(f.scene.PendingDirty(), Dirty::Composite));
    assert(!Has(f.scene.PendingDirty(), Dirty::Paint));
    assert(!Has(f.scene.PendingDirty(), Dirty::Layout));
    assert(f.scene.PixelsRevision() == pixels);
    assert(!f.scene.Build({17})); // Backdrop capability is surface metadata, not a paint change.
    const auto metadata_stats = f.scene.GetRenderStats();
    assert(metadata_stats.build_calls == stats.build_calls + 1);
    assert(metadata_stats.builds == stats.builds);
    assert(metadata_stats.layouts == stats.layouts);

    request = f.Request();
    assert(request.requires_backdrop);
    assert(f.scene.PreparePopupSurface(request, f.Configure(request)));
    assert(f.scene.SetProperty(muted, DslProperty::Visible, false));
    assert(f.scene.Build({17}));
    assert(!f.Request().requires_backdrop);
}

void OlderSubmissionAndParentGeneration()
{
    Fixture f;
    const auto old = f.Plan();
    assert(f.scene.SetBackground(old.request.active_node, {60, 70, 90, 255}));
    assert(f.scene.AdoptPopupSurface(old, Identity())); // Successful commits may trail UI values.
    const auto muted = Action(*old.input_snapshot, "muted");
    assert(f.scene.SetProperty(muted, DslProperty::Action, std::string("mute-new")));
    assert(!Click(f.scene, old, Identity(), "muted").control_edit);
    assert(f.scene.Build({17}));
    const auto newer = f.Plan();
    assert(f.scene.AdoptPopupSurface(newer, Identity(2)));
    const auto update = Click(f.scene, newer, Identity(2), "mute-new");
    assert(update.control_edit && update.control_edit->action == "mute-new");

    assert(f.scene.Build({17}));
    const auto request = f.Request(5);
    assert(request.parent_configure_generation == 5);
    assert(!f.scene.HasPopupSurfaceAdoption());
    assert(!f.scene.AdoptPopupSurface(newer, Identity(3)));
    assert(f.scene.RevokePopupSurface(Identity(2)));
    assert(f.scene.Build({17}));
    const auto current = f.Request(5);
    const auto plan = f.scene.PreparePopupSurface(current, f.Configure(current));
    assert(plan && f.scene.AdoptPopupSurface(*plan, Identity(3)));
}

Blueprint MenuLayout()
{
    auto root = Layout();
    auto &menu = root.children.back();
    menu.kind = Kind::Menu;
    menu.children.clear();
    auto item = Target("sort", 0, 36);
    item.kind = Kind::MenuItem;
    menu.children.push_back(std::move(item));
    auto submenu = Box(240, 160);
    submenu.kind = Kind::Menu;
    submenu.contour_recipe = AttachedPanelRecipe{12.0, 28.0, 16.0, ContourFallback::Detached};
    submenu.properties.push_back({DslProperty::PopupFor, std::string("sort")});
    submenu.properties.push_back({DslProperty::Background, panel_color});
    auto back = Box(0, 36);
    back.kind = Kind::MenuBack;
    submenu.children.push_back(std::move(back));
    root.children.push_back(std::move(submenu));
    return root;
}

void MenuReplacementAndEscape()
{
    Fixture f(MenuLayout());
    auto plan = f.Plan();
    auto identity = Identity();
    const auto menu_token = plan.request.popup_token;
    assert(f.scene.AdoptPopupSurface(plan, identity));
    const auto replaced = Click(f.scene, plan, identity, "sort");
    assert(replaced.changed && !replaced.activation);
    assert(!f.scene.HasPopupSurfaceAdoption() && f.scene.PopupToken() != menu_token);
    assert(!f.scene.AdoptPopupSurface(plan, Identity(2)));
    f.scene.HandlePopupSurfaceInput(contracts::FocusEvent{{17}, false, keyboard, false}, identity,
                                    plan.input_snapshot);
    assert(f.scene.PopupToken()); // A late old-surface focus loss cannot close its replacement.

    assert(f.scene.Build({17}));
    plan = f.Plan();
    identity = Identity(1, 52);
    const auto submenu_token = plan.request.popup_token;
    assert(plan.request.trigger == f.anchor);
    assert(f.scene.AdoptPopupSurface(plan, identity));
    Key(f.scene, plan, identity, 0x29, contracts::ButtonState::Pressed);
    assert(f.scene.PopupToken() && f.scene.PopupToken() != submenu_token);
    assert(!f.scene.HasPopupSurfaceAdoption());
    assert(f.scene.Build({17}));
    const auto parent = f.Plan();
    assert(parent.request.active_node != plan.request.active_node);
    assert(parent.request.trigger == f.anchor);
    assert(f.scene.AdoptPopupSurface(parent, Identity(1, 53)));
    Key(f.scene, parent, Identity(1, 53), 0x29, contracts::ButtonState::Released);
    Key(f.scene, parent, Identity(1, 53), 0x29, contracts::ButtonState::Pressed);
    assert(!f.scene.PopupToken() && !f.scene.HasPopupSurfaceAdoption());
}
} // namespace

int main()
{
    ProvenanceAndRootHandoff();
    InputUsesConfiguredGeometry();
    PaintingReusesLayout();
    ScrollUsesChildMetrics();
    FocusAndCloseScopes();
    BackdropCapability();
    OlderSubmissionAndParentGeneration();
    MenuReplacementAndEscape();
}
