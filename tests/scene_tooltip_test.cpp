#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr contracts::WindowId window{1};
constexpr contracts::InputSource pointer{1, 1, 1};
constexpr contracts::InputSource keyboard{1, 2, 1};
constexpr std::uint32_t escape_key = 0x29;
constexpr std::uint32_t tab_key = 0x2b;
constexpr std::uint64_t hover_delay_ns = 500'000'000;

// A deterministic Scene lifecycle fixture. It does not claim native input,
// actual font readability, GPU rendering or worker presentation validation.
ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * 7.0, font};
}

Blueprint Body()
{
    return ParseBlueprint(R"(
Card(height:168) {
    VStack(spacing:0) {
        TextField("Draft", action:"edit", height:36)
        Button("Save", action:"save", height:32)
        Button("Underlay", action:"underlay", height:60)
        Button("Menu", action:"menu", height:32)
    }
}
)");
}

Blueprint Layout()
{
    auto root = ParseBlueprint(R"(
VStack(spacing:0) {
    Card(height:168) { Button("Save placeholder", action:"save", height:32) }
    Popup("menu", width:160, height:80) {
        Button("Done", action:"done", height:32)
    }
    Tooltip("save", width:160, height:44, padding:8, tooltipDelayMs:500) {
        Text("Save document", font:14)
    }
}
)");
    auto body = Body();
    body.region = "body";
    body.region_mounted = true;
    root.children.front() = std::move(body);
    return root;
}

struct Fixture {
    Scene scene;
    std::shared_ptr<const InputSnapshot> shown;

    explicit Fixture(bool adopt = true, Blueprint blueprint = Layout())
        : scene(std::move(blueprint), Shape)
    {
        assert(scene.SetViewport({320, 280}));
        Prepare();
        if (adopt) {
            Adopt();
        }
    }

    void Prepare()
    {
        scene.Build(window);
        scene.AcknowledgeComposite();
        shown = scene.CaptureInputSnapshot();
        assert(shown);
    }

    void Adopt()
    {
        scene.ApplyInputSnapshot(shown);
        assert(scene.IsInputSnapshotAdopted(*shown));
    }

    void Present()
    {
        Prepare();
        Adopt();
    }

    contracts::NodeId Find(std::string_view action) const
    {
        for (const auto &node : shown->nodes) {
            if (node.id && node.action == action) {
                return node.id;
            }
        }
        assert(false && "Fixture action is missing");
        return {};
    }

    contracts::LogicalPoint Point(std::string_view action) const
    {
        const auto bounds = scene.Bounds(Find(action));
        return {bounds.x + 12, bounds.y + bounds.height / 2};
    }

    void Enter(std::uint64_t now, const std::shared_ptr<const InputSnapshot> &snapshot = {})
    {
        scene.HandleInput(contracts::PointerEnterEvent{window, Point("save"), now, pointer},
                          snapshot ? snapshot : shown);
    }

    void Motion(std::uint64_t now)
    {
        auto point = Point("save");
        point.x += 4;
        scene.HandleInput(contracts::PointerMotionEvent{window, point, now, pointer}, shown);
    }

    void Leave(std::uint64_t now)
    {
        scene.HandleInput(contracts::PointerLeaveEvent{window, now, pointer}, shown);
    }

    void Key(std::uint32_t key, bool down, std::uint64_t now, bool shift = false)
    {
        scene.HandleInput(contracts::KeyEvent{window,
                                              key,
                                              down ? contracts::ButtonState::Pressed
                                                   : contracts::ButtonState::Released,
                                              false,
                                              now,
                                              keyboard,
                                              {shift, false, false, false}},
                          shown);
    }

    void Tab(std::uint64_t now, bool shift = false)
    {
        Key(tab_key, true, now, shift);
        Key(tab_key, false, now, shift);
    }

    InteractionResult Button(contracts::LogicalPoint point, bool down, std::uint64_t now,
                             const std::shared_ptr<const InputSnapshot> &snapshot = {})
    {
        return scene.HandleInput(
            contracts::PointerButtonEvent{window, point, contracts::PointerButton::Primary,
                                          down ? contracts::ButtonState::Pressed
                                               : contracts::ButtonState::Released,
                                          0, now, pointer},
            snapshot ? snapshot : shown);
    }

    contracts::NodeId Open(std::uint64_t now)
    {
        Enter(now);
        assert(!scene.ReconcileTooltip(now));
        assert(scene.NextTooltipDeadlineNs() == now + hover_delay_ns);
        assert(scene.ReconcileTooltip(now + hover_delay_ns));
        const auto tooltip = scene.TooltipNode();
        assert(tooltip && scene.TooltipAnchor() == Find("save"));
        Present();
        assert(shown->tooltip_node == tooltip && shown->tooltip_anchor == Find("save"));
        return tooltip;
    }

    void ExpectHidden() const
    {
        assert(!scene.TooltipNode() && !scene.TooltipAnchor());
        assert(!scene.NextTooltipDeadlineNs());
    }
};

void CheckAdoptionAndOneShotDelay()
{
    Fixture f(false);
    constexpr std::uint64_t start = 1'000'000'000;
    f.Enter(start);
    assert(!f.scene.ReconcileTooltip(start));
    f.ExpectHidden(); // Preparing geometry alone cannot authorize a hover timer.

    f.Adopt();
    const auto anchor = f.Find("save");
    const auto bounds = f.scene.Bounds(anchor);
    const auto input_regions = f.scene.InputRegions();
    const auto epoch = f.scene.OwnerModalEpoch();
    assert(!f.scene.ReconcileTooltip(start));
    assert(f.scene.NextTooltipDeadlineNs() == start + hover_delay_ns);
    assert(!f.scene.ReconcileTooltip(start + hover_delay_ns - 1));
    assert(!f.scene.TooltipNode());

    f.Motion(start + 250'000'000);
    assert(!f.scene.ReconcileTooltip(start + 250'000'000));
    assert(f.scene.NextTooltipDeadlineNs() == start + hover_delay_ns);
    assert(f.scene.ReconcileTooltip(start + hover_delay_ns));
    const auto tooltip = f.scene.TooltipNode();
    assert(tooltip && f.scene.TooltipAnchor() == anchor);
    assert(!f.scene.NextTooltipDeadlineNs());

    f.Prepare();
    assert(!f.scene.IsInputSnapshotAdopted(*f.shown));
    assert(f.shown->tooltip_node == tooltip && f.shown->tooltip_anchor == anchor);
    assert(!f.shown->Find(tooltip)); // Passive metadata never creates an input target.
    for (const auto &node : f.shown->nodes) {
        for (const auto child : node.children) {
            assert(child != tooltip);
        }
    }
    assert(!f.scene.ReconcileTooltip(start + hover_delay_ns + 1));
    assert(f.scene.TooltipNode() == tooltip); // Keep presentation while its stamp is pending.
    f.Adopt();
    assert(!f.scene.ReconcileTooltip(start + 2 * hover_delay_ns));
    assert(f.scene.TooltipNode() == tooltip && !f.scene.NextTooltipDeadlineNs());
    assert(f.scene.Bounds(anchor) == bounds);
    assert(f.scene.InputRegions() == input_regions);
    assert(f.scene.OwnerModalEpoch() == epoch && !f.scene.PopupToken());
}

void CheckPassiveHitAndExistingFocus()
{
    Fixture f;
    f.Tab(1);
    const auto editor = f.Find("edit");
    assert(f.scene.State(editor).focused && f.scene.State(editor).focusVisible);
    f.scene.ReconcileTooltip(1);
    f.Present();
    const auto epoch = f.scene.OwnerModalEpoch();
    const auto tooltip = f.Open(100);
    assert(f.scene.State(editor).focused && f.scene.State(editor).focusVisible);
    assert(f.scene.OwnerModalEpoch() == epoch && !f.scene.PopupToken());

    const auto bounds = f.scene.Bounds(tooltip);
    const contracts::LogicalPoint point{bounds.x + 12, bounds.y + 8};
    const auto underlay = f.Find("underlay");
    const auto submitted_hit = f.scene.HitTest(point, *f.shown);
    const auto live_hit = f.scene.HitTest(point);
    assert(submitted_hit && submitted_hit->node == underlay);
    assert(live_hit && live_hit->node == underlay);
    assert(!f.Button(point, true, 1'000'000'000).activation);
    assert(!f.scene.TooltipNode());
    const auto up = f.Button(point, false, 1'000'000'001);
    assert(up.activation && up.activation->node == underlay && up.activation->action == "underlay");
    assert(f.scene.State(underlay).focused && !f.scene.State(editor).focused);
}

void CheckLeaveAndEscapeSuppression()
{
    Fixture f;
    f.Open(1);
    f.Leave(600'000'000);
    f.ExpectHidden();
    f.Present();

    const auto point = f.Point("save");
    assert(!f.Button(point, false, 600'000'001).activation);
    f.scene.HandleInput(contracts::PointerScrollEvent{window, point, 0, 1, 600'000'002, pointer},
                        f.shown);
    assert(!f.scene.ReconcileTooltip(1'200'000'000));
    f.ExpectHidden(); // A release or stale last-position scroll cannot restore presence.

    f.Open(1'300'000'000);
    f.Key(escape_key, true, 1'900'000'000);
    f.Key(escape_key, false, 1'900'000'001);
    f.ExpectHidden();
    f.Present();
    f.Motion(1'900'000'002);
    assert(!f.scene.ReconcileTooltip(2'500'000'000));
    f.ExpectHidden(); // Remaining over the same anchor does not undo Escape.

    f.Enter(2'600'000'000); // A fresh enter starts a distinct hover opportunity.
    assert(!f.scene.ReconcileTooltip(2'600'000'000));
    assert(f.scene.NextTooltipDeadlineNs() == 3'100'000'000);
    assert(f.scene.ReconcileTooltip(3'100'000'000));
    f.Present();
    f.scene.HandleInput(contracts::PointerCancelEvent{window, 3'100'000'001, pointer}, f.shown);
    f.ExpectHidden();
}

void CheckKeyboardFocusImmediateAndEscape()
{
    Fixture f;
    f.Tab(1);
    assert(f.scene.State(f.Find("edit")).focusVisible);
    assert(!f.scene.ReconcileTooltip(1));
    f.ExpectHidden();

    f.Tab(2);
    const auto anchor = f.Find("save");
    assert(f.scene.State(anchor).focused && f.scene.State(anchor).focusVisible);
    assert(f.scene.ReconcileTooltip(2)); // Keyboard focus has no hover delay.
    assert(f.scene.TooltipAnchor() == anchor && !f.scene.NextTooltipDeadlineNs());
    f.Present();
    f.Key(escape_key, true, 3);
    f.Key(escape_key, false, 4);
    f.Present();
    assert(!f.scene.ReconcileTooltip(1'000'000'000));
    f.ExpectHidden();
    assert(f.scene.State(anchor).focused); // Dismissal keeps the keyboard target.

    f.Tab(1'000'000'001);
    assert(f.scene.State(f.Find("underlay")).focused);
    assert(!f.scene.ReconcileTooltip(1'000'000'001));
    f.Tab(1'000'000'002, true);
    assert(f.scene.State(anchor).focused);
    assert(f.scene.ReconcileTooltip(1'000'000'002));
    assert(f.scene.TooltipAnchor() == anchor && !f.scene.NextTooltipDeadlineNs());
}

void CheckStaleSnapshotCannotArm()
{
    Fixture f;
    const auto previous = f.shown;
    assert(f.scene.SetViewport({400, 280}));
    f.Present();
    assert(!f.scene.IsInputSnapshotAdopted(*previous));
    assert(f.scene.IsInputSnapshotAdopted(*f.shown));
    f.Enter(1, previous);
    assert(!f.scene.ReconcileTooltip(1));
    assert(!f.scene.ReconcileTooltip(hover_delay_ns + 1));
    f.ExpectHidden();

    f.Enter(hover_delay_ns + 2);
    assert(!f.scene.ReconcileTooltip(hover_delay_ns + 2));
    assert(f.scene.NextTooltipDeadlineNs() == 2 * hover_delay_ns + 2);
    assert(f.scene.ReconcileTooltip(2 * hover_delay_ns + 2));
}

void CheckGeometryWaitsForNewAdoption()
{
    Fixture f;
    f.Open(1);
    const auto previous = f.shown;
    assert(f.scene.SetViewport({400, 300}));
    f.ExpectHidden();
    assert(!f.scene.ReconcileTooltip(600'000'000));
    f.ExpectHidden(); // Old adopted bounds cannot restart a timer while layout is dirty.

    f.Prepare();
    assert(f.shown->version > previous->version);
    assert(!f.scene.IsInputSnapshotAdopted(*f.shown));
    assert(!f.scene.ReconcileTooltip(700'000'000));
    f.ExpectHidden(); // New bounds need the worker's adoption receipt as well.
    f.Adopt();
    assert(!f.scene.ReconcileTooltip(800'000'000));
    assert(f.scene.NextTooltipDeadlineNs() == 1'300'000'000);
    assert(!f.scene.ReconcileTooltip(1'299'999'999));
    assert(f.scene.ReconcileTooltip(1'300'000'000));
    f.Present();
    assert(f.shown->tooltip_anchor == f.Find("save"));
}

void CheckModalAndPopupHide()
{
    Fixture f;
    f.Open(1);
    const auto token = f.scene.BeginOwnerModal(f.Find("underlay"), keyboard.seat);
    assert(token);
    f.scene.ReconcileTooltip(600'000'000);
    f.ExpectHidden();
    f.Present();
    f.Enter(600'000'001);
    assert(!f.scene.ReconcileTooltip(1'200'000'000));
    f.ExpectHidden();
    assert(f.scene.EndOwnerModal(*token));
    f.Present();

    f.Open(1'300'000'000);
    assert(f.scene.OpenPopup(f.Find("menu"), keyboard.seat));
    f.scene.ReconcileTooltip(1'900'000'000);
    f.ExpectHidden();
    f.Present();
    const auto popup_token = f.scene.PopupToken();
    assert(popup_token);
    f.Enter(1'900'000'001);
    assert(!f.scene.ReconcileTooltip(2'500'000'000));
    f.ExpectHidden();
    assert(f.scene.PopupToken() == popup_token);
    assert(f.scene.ClosePopup());
    f.Present();
    f.Open(2'600'000'000);
}

void CheckRegionReplacementRetiresOldInput()
{
    Fixture f;
    f.Open(1);
    const auto previous = f.shown;
    const auto old_anchor = f.Find("save");
    const auto old_point = f.Point("save");
    assert(!f.Button(old_point, true, 600'000'000).activation);
    assert(f.scene.State(old_anchor).captured);

    RegionUpdate update{"body", Body()};
    assert(f.scene.ReplaceRegions(std::span(&update, 1), BindingValues{}));
    f.ExpectHidden();
    assert(!f.scene.ReconcileTooltip(600'000'001));
    f.ExpectHidden();
    f.Present();
    const auto new_anchor = f.Find("save");
    assert(new_anchor != old_anchor);
    if (new_anchor.index == old_anchor.index) {
        assert(new_anchor.generation > old_anchor.generation);
    }
    assert(!f.shown->Find(old_anchor));
    assert(!f.scene.State(old_anchor).enabled);
    assert(!f.Button(old_point, false, 600'000'002, previous).activation);
    f.Enter(600'000'003, previous);
    assert(!f.scene.ReconcileTooltip(1'200'000'000));
    f.ExpectHidden();

    f.Open(1'300'000'000);
    assert(f.scene.TooltipAnchor() == new_anchor);
}

void CheckOwnerLossAndClose()
{
    Fixture f;
    f.Open(1);
    f.scene.HandleInput(contracts::FocusEvent{window, false, keyboard, false}, f.shown);
    f.scene.ReconcileTooltip(600'000'000);
    f.ExpectHidden();
    f.Present();
    f.Motion(600'000'001);
    assert(!f.scene.ReconcileTooltip(1'200'000'000));
    f.ExpectHidden();

    f.scene.HandleInput(contracts::FocusEvent{window, true, keyboard, false}, f.shown);
    f.Open(1'300'000'000);
    f.scene.HandleInput(contracts::CloseRequestedEvent{window}, f.shown);
    f.scene.ReconcileTooltip(1'900'000'000);
    f.ExpectHidden();
}

void CheckMonotonicAndSaturatingDeadline()
{
    Fixture f;
    constexpr std::uint64_t start = 1'000'000'000;
    f.Enter(start);
    assert(!f.scene.ReconcileTooltip(start));
    const auto deadline = f.scene.NextTooltipDeadlineNs();
    assert(deadline == start + hover_delay_ns);
    assert(!f.scene.ReconcileTooltip(0));
    assert(f.scene.NextTooltipDeadlineNs() == deadline);
    assert(!f.scene.ReconcileTooltip(*deadline - 1));
    assert(f.scene.ReconcileTooltip(*deadline));
    f.Present();
    const auto tooltip = f.scene.TooltipNode();
    assert(!f.scene.ReconcileTooltip(start));
    assert(f.scene.TooltipNode() == tooltip && !f.scene.NextTooltipDeadlineNs());

    Fixture saturated;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    saturated.Enter(maximum - 100);
    assert(!saturated.scene.ReconcileTooltip(maximum - 100));
    assert(saturated.scene.NextTooltipDeadlineNs() == maximum);
    assert(!saturated.scene.ReconcileTooltip(maximum - 1));
    assert(saturated.scene.ReconcileTooltip(maximum));
    saturated.Present();
    assert(!saturated.scene.ReconcileTooltip(maximum));
    assert(saturated.scene.TooltipNode() && !saturated.scene.NextTooltipDeadlineNs());
}

Blueprint LayoutWithOtherTooltip()
{
    auto root = Layout();
    auto fragment = ParseBlueprint(R"(
Card {
    Button("Other placeholder", action:"underlay", height:32)
    Tooltip("underlay", width:160, height:44, padding:8, tooltipDelayMs:500) {
        Text("Other command", font:14)
    }
}
)");
    root.children.push_back(std::move(fragment.children.back()));
    return root;
}

void CheckSuppressionSurvivesUnrelatedLayout(bool use_escape)
{
    Fixture f;
    f.Open(1);
    const auto anchor = f.Find("save");
    const auto bounds = f.scene.Bounds(anchor);
    if (use_escape) {
        f.Key(escape_key, true, 600'000'000);
        f.Key(escape_key, false, 600'000'001);
    } else {
        const auto point = f.Point("save");
        f.Button(point, true, 600'000'000);
        const auto up = f.Button(point, false, 600'000'001);
        assert(up.activation && up.activation->action == "save");
    }
    f.ExpectHidden();
    f.Present();

    // This changes the scene layout while leaving this anchor and hover intact.
    assert(f.scene.SetProperty(f.Find("underlay"), DslProperty::Height, 64.0));
    assert(!f.scene.ReconcileTooltip(700'000'000));
    f.ExpectHidden();
    f.Prepare();
    assert(f.scene.Bounds(anchor) == bounds);
    assert(!f.scene.IsInputSnapshotAdopted(*f.shown));
    assert(!f.scene.ReconcileTooltip(800'000'000));
    f.ExpectHidden();
    f.Adopt();
    f.Motion(800'000'001);
    assert(!f.scene.ReconcileTooltip(1'400'000'000));
    f.ExpectHidden(); // Adopting unrelated layout must not undo click or Escape.

    f.Enter(1'500'000'000);
    assert(!f.scene.ReconcileTooltip(1'500'000'000));
    assert(f.scene.NextTooltipDeadlineNs() == 2'000'000'000);
    assert(f.scene.ReconcileTooltip(2'000'000'000));
}

void CheckPendingDeadlineSurvivesUnrelatedLayout()
{
    Fixture f;
    constexpr std::uint64_t start = 1'000'000'000;
    const auto anchor = f.Find("save");
    const auto bounds = f.scene.Bounds(anchor);
    f.Enter(start);
    assert(!f.scene.ReconcileTooltip(start));
    const auto deadline = f.scene.NextTooltipDeadlineNs();
    assert(deadline == start + hover_delay_ns);

    assert(f.scene.SetProperty(f.Find("underlay"), DslProperty::Height, 64.0));
    assert(!f.scene.ReconcileTooltip(start + 100'000'000));
    f.ExpectHidden(); // The timer is fenced rather than available to the poll loop.
    f.Prepare();
    assert(f.scene.Bounds(anchor) == bounds);
    assert(!f.scene.ReconcileTooltip(start + 200'000'000));
    f.ExpectHidden();
    f.Adopt();
    assert(f.scene.NextTooltipDeadlineNs() == deadline);
    assert(!f.scene.ReconcileTooltip(start + 300'000'000));
    assert(f.scene.NextTooltipDeadlineNs() == deadline);
    assert(!f.scene.ReconcileTooltip(*deadline - 1));
    assert(f.scene.ReconcileTooltip(*deadline)); // Preparation did not add another 500 ms.
    assert(f.scene.TooltipAnchor() == anchor);
}

void CheckKeyboardCandidateWinsOverStationaryHover()
{
    Fixture f(true, LayoutWithOtherTooltip());
    f.Enter(1);
    assert(!f.scene.ReconcileTooltip(1));
    assert(f.scene.NextTooltipDeadlineNs() == hover_delay_ns + 1);
    f.Tab(2); // Editor, then Save, then a different explained keyboard target.
    f.Tab(3);
    f.Tab(4);
    const auto save = f.Find("save");
    const auto other = f.Find("underlay");
    assert(f.scene.State(save).hovered);
    assert(f.scene.State(other).focused && f.scene.State(other).focusVisible);
    assert(f.scene.ReconcileTooltip(4));
    assert(f.scene.TooltipAnchor() == other && !f.scene.NextTooltipDeadlineNs());
    f.Present();
    assert(f.shown->tooltip_anchor == other);
    assert(f.scene.State(save).hovered && f.scene.State(other).focused);

    f.Tab(5, true);
    f.Present(); // Withdraw the old passive stamp before showing the new one.
    assert(f.scene.State(save).focused && f.scene.State(save).focusVisible);
    assert(f.scene.ReconcileTooltip(5));
    assert(f.scene.TooltipAnchor() == save && !f.scene.NextTooltipDeadlineNs());

    Fixture legacy_focus;
    legacy_focus.Enter(1);
    assert(!legacy_focus.scene.ReconcileTooltip(1));
    assert(legacy_focus.scene.FocusNext());
    assert(!legacy_focus.scene.ReconcileTooltip(2));
    legacy_focus.ExpectHidden(); // A focused unannotated editor replaces mouse explanation.
    assert(legacy_focus.scene.FocusNext());
    assert(legacy_focus.scene.ReconcileTooltip(3));
    assert(legacy_focus.scene.TooltipAnchor() == legacy_focus.Find("save"));
}

void CheckDirectBuildWithdrawsUnavailableTooltip()
{
    Fixture disabled;
    disabled.Open(1);
    const auto anchor = disabled.Find("save");
    assert(disabled.scene.SetEnabled(anchor, false));
    disabled.Prepare(); // No explicit ReconcileTooltip between disable and Build.
    disabled.ExpectHidden();
    assert(!disabled.shown->tooltip_node && !disabled.shown->tooltip_anchor);
    assert(disabled.shown->Find(anchor) && !disabled.shown->Find(anchor)->enabled);
    disabled.Adopt();
    assert(!disabled.scene.ReconcileTooltip(600'000'000));
    disabled.ExpectHidden();

    Fixture modal;
    const auto tooltip = modal.Open(1);
    const auto epoch = modal.scene.OwnerModalEpoch();
    assert(!modal.scene.BeginOwnerModal(tooltip, keyboard.seat));
    assert(!modal.scene.OwnerModalToken() && modal.scene.OwnerModalEpoch() == epoch);
    assert(modal.scene.TooltipNode() == tooltip);

    const auto token = modal.scene.BeginOwnerModal(modal.Find("underlay"), keyboard.seat);
    assert(token);
    modal.Prepare(); // CancelInput/Build withdraw without waiting for a timer reconciliation.
    modal.ExpectHidden();
    assert(!modal.shown->tooltip_node && !modal.shown->tooltip_anchor);
    assert(modal.shown->owner_modal_epoch == *token);
    modal.Adopt();
    assert(modal.scene.OwnerModalToken() == *token);
}

} // namespace

int main()
{
    CheckAdoptionAndOneShotDelay();
    CheckPassiveHitAndExistingFocus();
    CheckLeaveAndEscapeSuppression();
    CheckKeyboardFocusImmediateAndEscape();
    CheckStaleSnapshotCannotArm();
    CheckGeometryWaitsForNewAdoption();
    CheckModalAndPopupHide();
    CheckRegionReplacementRetiresOldInput();
    CheckOwnerLossAndClose();
    CheckMonotonicAndSaturatingDeadline();
    CheckSuppressionSurvivesUnrelatedLayout(true);
    CheckSuppressionSurvivesUnrelatedLayout(false);
    CheckPendingDeadlineSurvivesUnrelatedLayout();
    CheckKeyboardCandidateWinsOverStationaryHover();
    CheckDirectBuildWithdrawsUnavailableTooltip();
}
