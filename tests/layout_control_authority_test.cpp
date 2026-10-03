#include "prism/wm/layout_control.hpp"
#include <cassert>
#include <iostream>

using namespace prism;
using namespace prism::contracts;
using namespace prism::wm;

namespace {
constexpr std::uint64_t Now = 1'000'000'000;
const LayoutControlPrincipal Topbar{{21}, {31}, 1234, WindowRole::TopBar, true};
const LayoutControlPrincipal Controls{{22}, {32}, 1235, WindowRole::LayoutControls, true};

LayoutSnapshot Snapshot()
{
    LayoutSnapshot value;
    value.session = 1;
    value.revision = value.topology_revision = value.layout_revision = value.focus_revision = 1;
    value.outputs.push_back({2, "Screen", {0, 0, 800, 600}, 1, true, true});
    value.workspaces.push_back({3, 4, 2, "Main", true});
    value.boundaries.push_back(
        {5, 4, 6, 7, 3, LayoutBoundaryAxis::X, {395, 0, 10, 600}, true, true});
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

LayoutControlRequest BoundaryBegin(std::uint64_t id = 1)
{
    auto request = Begin(id);
    request.operation = LayoutControlOperation::BoundaryGesture;
    request.target.boundary = 5;
    return request;
}

class PreviewApplier : public LayoutControlApplier {
public:
    explicit PreviewApplier(LayoutSnapshot &snapshot) : snapshot_(snapshot)
    {
    }

    LayoutControlError TrackLayoutIntent(const LayoutControlRequest &request,
                                         LayoutControlResult &result) override
    {
        assert(request.operation == LayoutControlOperation::BoundaryGesture);
        if (request.phase == LayoutControlPhase::Begin) {
            ++begins;
        } else if (request.phase == LayoutControlPhase::Update) {
            ++updates;
        } else {
            assert(request.phase == LayoutControlPhase::Cancel);
            ++cancels;
        }
        if (tracking_error != LayoutControlError::None) {
            return tracking_error;
        }

        Advance(result);
        return LayoutControlError::None;
    }

    LayoutControlError ApplyLayoutIntent(const LayoutControlRequest &request,
                                         LayoutControlResult &result) override
    {
        assert(request.intent == LayoutControlIntent::ApplyBoundary);
        ++ends;
        Advance(result);
        result.applied = true;
        return LayoutControlError::None;
    }

    unsigned begins{}, updates{}, cancels{}, ends{};
    LayoutControlError tracking_error{};

private:
    void Advance(LayoutControlResult &result)
    {
        ++snapshot_.revision;
        ++snapshot_.layout_revision;
        result.revision = snapshot_.revision;
        result.layout_revision = snapshot_.layout_revision;
    }

    LayoutSnapshot &snapshot_;
};

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

void UnsupportedAndReplayBound()
{
    LayoutControlAuthority authority;
    auto snapshot = Snapshot();
    auto begin = Begin();
    authority.RecordInput(Topbar, begin.input, Now);
    const auto began = authority.Apply(Topbar, begin, snapshot, Now);
    assert(began.status == LayoutControlStatus::Began);
    auto end = Next(begin, began, LayoutControlPhase::End);
    end.intent = LayoutControlIntent::EnterImmersive;
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

void BoundaryRolesAndProof()
{
    for (const auto role :
         {WindowRole::Toplevel, WindowRole::Desktop, WindowRole::TopBar, WindowRole::Dock}) {
        LayoutControlAuthority authority;
        auto snapshot = Snapshot();
        PreviewApplier applier(snapshot);
        auto principal = Controls;
        principal.role = role;
        const auto request = BoundaryBegin();
        authority.RecordInput(principal, request.input, Now, 5);
        assert(authority.Apply(principal, request, snapshot, Now, &applier).error ==
               LayoutControlError::Unauthorized);
        assert(applier.begins == 0);
    }

    LayoutControlAuthority authority;
    auto snapshot = Snapshot();
    PreviewApplier applier(snapshot);
    const auto group = Begin();
    authority.RecordInput(Controls, group.input, Now);
    assert(authority.Apply(Controls, group, snapshot, Now, &applier).error ==
           LayoutControlError::Unauthorized);

    auto request = BoundaryBegin(10);
    // A serial from another handle cannot authorize this boundary, even for
    // the same registered Shell process and the same current layout.
    authority.RecordInput(Controls, request.input, Now, 9);
    assert(authority.Apply(Controls, request, snapshot, Now, &applier).error ==
           LayoutControlError::InvalidInput);
    assert(applier.begins == 0);

    request.request++;
    authority.RecordInput(Controls, request.input, Now, 5);
    snapshot.boundaries[0].resizable = false;
    assert(authority.Apply(Controls, request, snapshot, Now, &applier).error ==
           LayoutControlError::StaleTarget);
    snapshot.boundaries[0].resizable = true;
    request.request++;
    assert(authority.Apply(Controls, request, snapshot, Now).error ==
           LayoutControlError::Unsupported);
    request.request++;
    assert(authority.Apply(Controls, request, snapshot, Now, &applier).error ==
           LayoutControlError::InvalidInput);
}

void BoundaryPreviewRevisions()
{
    LayoutControlAuthority authority;
    auto snapshot = Snapshot();
    PreviewApplier applier(snapshot);
    const auto begin = BoundaryBegin();
    authority.RecordInput(Controls, begin.input, Now, 5);
    const auto began = authority.Apply(Controls, begin, snapshot, Now, &applier);
    assert(began.status == LayoutControlStatus::Began && began.layout_revision == 2);
    authority.Reconcile(snapshot, Now);
    assert(authority.TakeNotifications().empty());
    assert(authority.Apply(Controls, begin, snapshot, Now, &applier) == began);
    assert(applier.begins == 1);

    auto update = Next(begin, began, LayoutControlPhase::Update);
    update.position.x = 150;
    const auto updated = authority.Apply(Controls, update, snapshot, Now, &applier);
    assert(updated.status == LayoutControlStatus::Updated && updated.layout_revision == 3);
    assert(!updated.applied && update.target.layout_revision == 1);
    assert(authority.Apply(Controls, update, snapshot, Now, &applier) == updated);
    assert(applier.updates == 1);

    ++snapshot.revision;
    ++snapshot.focus_revision;
    authority.Reconcile(snapshot, Now);
    assert(authority.TakeNotifications().empty());
    update = Next(update, updated, LayoutControlPhase::Update);
    update.position.x = 170;
    const auto advanced = authority.Apply(Controls, update, snapshot, Now, &applier);
    assert(advanced.status == LayoutControlStatus::Updated && advanced.layout_revision == 4);

    auto end = Next(update, advanced, LayoutControlPhase::End);
    end.intent = LayoutControlIntent::ApplyBoundary;
    const auto ended = authority.Apply(Controls, end, snapshot, Now, &applier);
    assert(ended.status == LayoutControlStatus::Ended && ended.applied);
    assert(ended.layout_revision == 5 && applier.ends == 1);
    assert(authority.Apply(Controls, end, snapshot, Now, &applier) == ended);
    assert(applier.ends == 1 && authority.TakeNotifications().empty());
}

void BoundaryExternalCancellation()
{
    for (unsigned reason = 0; reason < 6; ++reason) {
        LayoutControlAuthority authority;
        auto snapshot = Snapshot();
        PreviewApplier applier(snapshot);
        const auto begin = BoundaryBegin();
        authority.RecordInput(Controls, begin.input, Now, 5);
        const auto began = authority.Apply(Controls, begin, snapshot, Now, &applier);
        auto update = Next(begin, began, LayoutControlPhase::Update);
        const auto updated = authority.Apply(Controls, update, snapshot, Now, &applier);
        assert(updated.status == LayoutControlStatus::Updated);

        if (reason == 0) {
            ++snapshot.layout_revision;
        } else if (reason == 1) {
            ++snapshot.topology_revision;
        } else if (reason == 2) {
            snapshot.boundaries[0].resizable = false;
        } else if (reason == 3) {
            snapshot.boundaries.clear();
        } else if (reason == 4) {
            snapshot.workspaces[0].active = false;
        } else {
            authority.CancelInput(LayoutInputKind::Pointer, 0);
        }
        authority.Reconcile(snapshot, Now);
        const auto notifications = authority.TakeNotifications();
        assert(notifications.size() == 1);
        assert(notifications[0].principal == Controls);
        assert(notifications[0].result.status == LayoutControlStatus::Cancelled);
        assert(notifications[0].result.session == began.session);
        assert(authority.Apply(Controls, begin, snapshot, Now, &applier).status ==
               LayoutControlStatus::Cancelled);
        assert(authority.Apply(Controls, update, snapshot, Now, &applier).status ==
               LayoutControlStatus::Cancelled);
        update = Next(update, updated, LayoutControlPhase::Update);
        assert(authority.Apply(Controls, update, snapshot, Now, &applier).error ==
               LayoutControlError::UnknownSession);
        assert(applier.begins == 1 && applier.updates == 1);
    }
}

void BoundaryTerminalAndReplayConflict()
{
    for (unsigned terminal = 0; terminal < 4; ++terminal) {
        LayoutControlAuthority authority;
        auto snapshot = Snapshot();
        PreviewApplier applier(snapshot);
        const auto begin = BoundaryBegin();
        authority.RecordInput(Controls, begin.input, Now, 5);
        const auto began = authority.Apply(Controls, begin, snapshot, Now, &applier);
        const auto update = Next(begin, began, LayoutControlPhase::Update);
        const auto updated = authority.Apply(Controls, update, snapshot, Now, &applier);
        auto request = Next(update, updated, LayoutControlPhase::Cancel);
        if (terminal == 0 || terminal == 1) {
            if (terminal == 1) {
                request.phase = LayoutControlPhase::End; // No ApplyBoundary commits nothing.
            }
            const auto result = authority.Apply(Controls, request, snapshot, Now, &applier);
            assert(result.status ==
                   (terminal ? LayoutControlStatus::Ended : LayoutControlStatus::Cancelled));
            assert(!result.applied && applier.cancels == 1 && applier.ends == 0);
            assert(authority.Apply(Controls, request, snapshot, Now, &applier) == result);
            assert(applier.cancels == 1);
        } else {
            request = update;
            if (terminal == 2) {
                ++request.position.x; // Same request ID with altered payload.
            } else {
                ++request.request;
                ++request.sequence;
                request.target.layout_revision = snapshot.layout_revision;
            }
            const auto result = authority.Apply(Controls, request, snapshot, Now, &applier);
            assert(result.error == (terminal == 2 ? LayoutControlError::InvalidSequence
                                                  : LayoutControlError::StaleTarget));
            assert(authority.TakeNotifications().size() == 1);
            assert(authority.Apply(Controls, begin, snapshot, Now, &applier).status ==
                   LayoutControlStatus::Cancelled);
        }
        request = Next(request, updated, LayoutControlPhase::Update);
        request.request += 100;
        assert(authority.Apply(Controls, request, snapshot, Now, &applier).error ==
               LayoutControlError::UnknownSession);
    }
}
} // namespace

int main()
{
    AuthorizationAndSequence();
    InvalidContinuationTerminates();
    RevisionsAndCancellation();
    ContactsBusyAndRelease();
    UnsupportedAndReplayBound();
    BoundaryRolesAndProof();
    BoundaryPreviewRevisions();
    BoundaryExternalCancellation();
    BoundaryTerminalAndReplayConflict();
    std::cout << "WM layout control authority passed\n";
}
