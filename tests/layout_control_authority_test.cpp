#include "prism/wm/layout_control.hpp"
#include <cassert>
#include <iostream>

using namespace prism;
using namespace prism::contracts;
using namespace prism::wm;

namespace {
constexpr std::uint64_t Now = 1'000'000'000;
const LayoutControlPrincipal Topbar{{21}, {31}, 1234, WindowRole::TopBar, true};

LayoutSnapshot Snapshot()
{
    LayoutSnapshot value;
    value.session = 1;
    value.revision = value.topology_revision = value.layout_revision = value.focus_revision = 1;
    value.outputs.push_back({2, "Screen", {0, 0, 800, 600}, 1, true, true});
    value.workspaces.push_back({3, 4, 2, "Main", true});
    value.boundaries.push_back(
        {5, 4, 6, 7, 3, LayoutBoundaryAxis::X, {395, 0, 10, 600}, true, false});
    return value;
}

LayoutControlRequest Begin(std::uint64_t id = 1)
{
    LayoutControlRequest request;
    request.request = id;
    request.gesture = id;
    request.sequence = 1;
    request.target = {1, 2, 3, 4, 0, 1, 1};
    request.input = {LayoutInputKind::Pointer, 100, 0};
    request.position = {100, 10};
    return request;
}

LayoutControlRequest Next(LayoutControlRequest request, const LayoutControlResult &result,
                          LayoutControlPhase phase)
{
    request.request++;
    request.sequence++;
    request.session = result.session;
    request.phase = phase;
    return request;
}

void AuthorizationAndSequence()
{
    LayoutControlAuthority authority;
    const auto snapshot = Snapshot();
    auto request = Begin();
    auto forged = Topbar;
    forged.instance = {99};
    authority.RecordInput(Topbar, request.input, Now);
    assert(authority.Apply(forged, request, snapshot, Now).error ==
           LayoutControlError::InvalidInput);
    auto ordinary = Topbar;
    ordinary.role = WindowRole::Toplevel;
    assert(authority.Apply(ordinary, request, snapshot, Now).error ==
           LayoutControlError::Unauthorized);
    auto unmapped = Topbar;
    unmapped.mapped = false;
    assert(authority.Apply(unmapped, request, snapshot, Now).error ==
           LayoutControlError::Unauthorized);
    auto invalid = request;
    invalid.request = 100;
    invalid.input.serial++;
    assert(authority.Apply(Topbar, invalid, snapshot, Now).error ==
           LayoutControlError::InvalidInput);

    const auto began = authority.Apply(Topbar, request, snapshot, Now);
    assert(began.status == LayoutControlStatus::Began && began.session && !began.applied);
    assert(authority.Apply(Topbar, request, snapshot, Now) == began);
    invalid = Begin(101);
    assert(authority.Apply(Topbar, invalid, snapshot, Now).error ==
           LayoutControlError::InvalidInput);

    auto update = Next(request, began, LayoutControlPhase::Update);
    update.position.x = 130;
    assert(authority.Apply(forged, update, snapshot, Now).error ==
           LayoutControlError::Unauthorized);
    const auto updated = authority.Apply(Topbar, update, snapshot, Now);
    assert(updated.status == LayoutControlStatus::Updated && !updated.applied);
    auto end = Next(update, updated, LayoutControlPhase::End);
    const auto ended = authority.Apply(Topbar, end, snapshot, Now);
    assert(ended.status == LayoutControlStatus::Ended && !ended.applied);
    assert(authority.Apply(Topbar, end, snapshot, Now) == ended);
    update = Next(end, ended, LayoutControlPhase::Update);
    assert(authority.Apply(Topbar, update, snapshot, Now).error ==
           LayoutControlError::UnknownSession);
}

void InvalidContinuationTerminates()
{
    for (int invalidKind : {0, 1, 2}) {
        LayoutControlAuthority authority;
        const auto snapshot = Snapshot();
        const auto begin = Begin();
        authority.RecordInput(Topbar, begin.input, Now);
        const auto began = authority.Apply(Topbar, begin, snapshot, Now);
        auto invalid = Next(begin, began, LayoutControlPhase::Update);
        if (invalidKind == 1) {
            invalid.sequence++;
        } else if (invalidKind == 2) {
            invalid = begin;
            invalid.position.x++;
        } else {
            invalid.target.root++;
        }
        const auto result = authority.Apply(Topbar, invalid, snapshot, Now);
        assert(result.error == (invalidKind ? LayoutControlError::InvalidSequence
                                            : LayoutControlError::StaleTarget));
        assert(authority.TakeNotifications().size() == 1);
        auto retry = Next(begin, began, LayoutControlPhase::Update);
        retry.request += 100;
        assert(authority.Apply(Topbar, retry, snapshot, Now).error ==
               LayoutControlError::UnknownSession);
    }
}

void RevisionsAndCancellation()
{
    for (int scenario = 0; scenario < 8; ++scenario) {
        LayoutControlAuthority authority;
        auto snapshot = Snapshot();
        auto request = Begin();
        authority.RecordInput(Topbar, request.input, Now);
        const auto began = authority.Apply(Topbar, request, snapshot, Now);
        assert(began.status == LayoutControlStatus::Began);
        snapshot.revision++;
        snapshot.focus_revision++;
        authority.Reconcile(snapshot, Now);
        assert(authority.TakeNotifications().empty());
        switch (scenario) {
        case 0:
            snapshot.topology_revision++;
            break;
        case 1:
            snapshot.layout_revision++;
            break;
        case 2:
            snapshot.workspaces[0].active = false;
            break;
        case 3:
            snapshot.outputs.clear();
            break;
        case 4:
            snapshot.session++;
            break;
        case 5:
            authority.CancelInput(LayoutInputKind::Pointer, 0);
            break;
        case 6:
            authority.Revoke(Topbar.instance);
            break;
        case 7:
            break;
        }
        authority.Reconcile(snapshot, scenario == 7 ? Now + 31'000'000'000 : Now);
        const auto notifications = authority.TakeNotifications();
        assert(notifications.size() == 1);
        assert(notifications[0].principal == Topbar);
        assert(notifications[0].result.status == LayoutControlStatus::Cancelled);
        assert(notifications[0].result.session == began.session);
        assert(authority.TakeNotifications().empty());
        assert(authority.Apply(Topbar, request, snapshot, Now).status !=
               LayoutControlStatus::Began);
    }
}

void ContactsBusyAndRelease()
{
    LayoutControlAuthority authority;
    auto snapshot = Snapshot();
    auto pointer = Begin();
    authority.RecordInput(Topbar, pointer.input, Now);
    // Wayland Up may arrive before the worker's queued Begin. Grace is bounded.
    authority.ReleaseInput(LayoutInputKind::Pointer, 0, Now + 100);
    const auto began = authority.Apply(Topbar, pointer, snapshot, Now + 200);
    assert(began.status == LayoutControlStatus::Began);
    auto touch = Begin(20);
    touch.input = {LayoutInputKind::Touch, 201, 7};
    authority.RecordInput(Topbar, touch.input, Now + 300);
    assert(authority.Apply(Topbar, touch, snapshot, Now + 300).error == LayoutControlError::Busy);
    authority.CancelInput(LayoutInputKind::Touch, 7);
    assert(authority.TakeNotifications().empty());
    authority.Reconcile(snapshot, Now + 3'000'000'000);
    const auto notifications = authority.TakeNotifications();
    assert(notifications.size() == 1 &&
           notifications[0].result.error == LayoutControlError::Expired);

    authority.RecordInput(Topbar, touch.input, Now + 3'000'000'001);
    touch.request++;
    const auto touched = authority.Apply(Topbar, touch, snapshot, Now + 3'000'000'002);
    assert(touched.status == LayoutControlStatus::Began && touched.session != began.session);
    auto wrongContact = Next(touch, touched, LayoutControlPhase::Update);
    wrongContact.input.contact++;
    assert(authority.Apply(Topbar, wrongContact, snapshot, Now + 3'000'000'003).error ==
           LayoutControlError::StaleTarget);
    authority.CancelInput(LayoutInputKind::Touch, 7);
    assert(authority.TakeNotifications().size() == 1);
}

void BoundariesUnsupportedAndReplayBound()
{
    LayoutControlAuthority authority;
    auto snapshot = Snapshot();
    auto begin = Begin();
    begin.operation = LayoutControlOperation::BoundaryGesture;
    begin.target.boundary = 5;
    authority.RecordInput(Topbar, begin.input, Now);
    const auto began = authority.Apply(Topbar, begin, snapshot, Now);
    assert(began.status == LayoutControlStatus::Began);
    auto end = Next(begin, began, LayoutControlPhase::End);
    end.intent = LayoutControlIntent::ApplyBoundary;
    const auto result = authority.Apply(Topbar, end, snapshot, Now);
    assert(result.status == LayoutControlStatus::Rejected &&
           result.error == LayoutControlError::Unsupported && !result.applied);
    assert(authority.Apply(Topbar, end, snapshot, Now) == result);
    for (std::uint64_t id = 100; id < 400; ++id) {
        auto bad = Begin(id);
        bad.input.serial = 404;
        assert(authority.Apply(Topbar, bad, snapshot, Now).error ==
               LayoutControlError::InvalidInput);
    }
    assert(authority.Apply(Topbar, begin, snapshot, Now).error == LayoutControlError::InvalidInput);
    auto stale = Begin(500);
    stale.target.wm_session++;
    assert(authority.Apply(Topbar, stale, snapshot, Now).error == LayoutControlError::StaleSession);
    stale.request++;
    stale.target.wm_session--;
    snapshot.outputs[0].supported = false;
    assert(authority.Apply(Topbar, stale, snapshot, Now).error == LayoutControlError::Unsupported);
}
} // namespace

int main()
{
    AuthorizationAndSequence();
    InvalidContinuationTerminates();
    RevisionsAndCancellation();
    ContactsBusyAndRelease();
    BoundariesUnsupportedAndReplayBound();
    std::cout << "WM layout control authority passed\n";
}
