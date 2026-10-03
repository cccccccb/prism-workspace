#include "prism/wm/layout_control.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace prism::wm {
namespace {
using namespace contracts;

constexpr std::uint64_t ReleaseGrace = 2'000'000'000;
constexpr std::uint64_t IdleTimeout = 30'000'000'000;
constexpr std::uint64_t Lifetime = 120'000'000'000;
constexpr std::size_t MaxInputs = 64;
constexpr std::size_t MaxJournal = 256;

bool Elapsed(std::uint64_t now, std::uint64_t start, std::uint64_t limit)
{
    return now < start || now - start > limit;
}

bool Authorized(const LayoutControlPrincipal &principal)
{
    return principal.instance.value && principal.pid && principal.mapped &&
           (principal.role == WindowRole::TopBar || principal.role == WindowRole::LayoutControls);
}

LayoutControlResult Result(const LayoutControlRequest &request, const LayoutSnapshot &snapshot,
                           LayoutControlStatus status, LayoutControlError error = {})
{
    return {request.request,
            request.gesture,
            request.session,
            request.sequence,
            status,
            error,
            snapshot.revision,
            snapshot.topology_revision,
            snapshot.layout_revision,
            request.position,
            false};
}

LayoutControlError ValidateTarget(const LayoutControlRequest &request,
                                  const LayoutSnapshot &snapshot)
{
    const auto &target = request.target;
    if (target.wm_session != snapshot.session) {
        return LayoutControlError::StaleSession;
    }
    if (target.topology_revision != snapshot.topology_revision ||
        target.layout_revision != snapshot.layout_revision) {
        return LayoutControlError::StaleLayout;
    }
    const auto output = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
                                     [&target](const auto &o) { return o.id == target.output; });
    if (output == snapshot.outputs.end()) {
        return LayoutControlError::StaleTarget;
    }
    if (!output->supported) {
        return LayoutControlError::Unsupported;
    }
    const auto workspace =
        std::find_if(snapshot.workspaces.begin(), snapshot.workspaces.end(),
                     [&target](const auto &w) { return w.id == target.workspace; });
    if (workspace == snapshot.workspaces.end() || !workspace->active || !target.root ||
        workspace->root != target.root || workspace->output != target.output) {
        return LayoutControlError::StaleTarget;
    }
    if (request.operation == LayoutControlOperation::GroupGesture) {
        return target.boundary ? LayoutControlError::StaleTarget : LayoutControlError::None;
    }
    const auto boundary =
        std::find_if(snapshot.boundaries.begin(), snapshot.boundaries.end(),
                     [&target](const auto &b) { return b.id == target.boundary; });
    if (boundary == snapshot.boundaries.end() || !boundary->visible || !boundary->resizable ||
        boundary->workspace != target.workspace) {
        return LayoutControlError::StaleTarget;
    }
    return LayoutControlError::None;
}
} // namespace

void LayoutControlAuthority::RecordInput(const LayoutControlPrincipal &principal,
                                         const contracts::LayoutInputProof &proof,
                                         std::uint64_t now, std::uint64_t boundary)
{
    CancelInput(proof.kind, proof.contact);
    if (!Authorized(principal) || !proof.serial) {
        return;
    }
    if (inputs_.size() == MaxInputs) {
        const auto oldest = inputs_.front().proof;
        CancelInput(oldest.kind, oldest.contact);
    }
    inputs_.push_back({principal, proof, now, 0, boundary, false});
}

void LayoutControlAuthority::ReleaseInput(contracts::LayoutInputKind kind, std::int32_t contact,
                                          std::uint64_t now)
{
    for (auto &input : inputs_) {
        if (input.proof.kind == kind && input.proof.contact == contact && !input.released) {
            input.released = now;
        }
    }
}

void LayoutControlAuthority::CancelInput(contracts::LayoutInputKind kind, std::int32_t contact)
{
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const auto &proof = it->second.last.input;
        if (proof.kind == kind && proof.contact == contact) {
            Cancel(it++, contracts::LayoutControlError::InvalidInput);
        } else {
            ++it;
        }
    }
    std::erase_if(inputs_, [kind, contact](const auto &input) {
        return input.proof.kind == kind && input.proof.contact == contact;
    });
}

void LayoutControlAuthority::CancelInstance(contracts::InstanceId instance,
                                            contracts::LayoutControlError error)
{
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.principal.instance == instance) {
            Cancel(it++, error);
        } else {
            ++it;
        }
    }
    std::erase_if(inputs_,
                  [instance](const auto &input) { return input.principal.instance == instance; });
}

void LayoutControlAuthority::Revoke(contracts::InstanceId instance)
{
    CancelInstance(instance, contracts::LayoutControlError::Disconnected);
    std::erase_if(journal_,
                  [instance](const auto &entry) { return entry.principal.instance == instance; });
}

void LayoutControlAuthority::Reset()
{
    inputs_.clear();
    sessions_.clear();
    journal_.clear();
    notifications_.clear();
    revisions_ = {};
    // Never reuse a previously issued session ID while this WM process is alive.
}

contracts::LayoutControlResult LayoutControlAuthority::Apply(
    const LayoutControlPrincipal &principal, const contracts::LayoutControlRequest &request,
    const contracts::LayoutSnapshot &snapshot, std::uint64_t now, LayoutControlApplier *applier)
{
    using enum contracts::LayoutControlError;
    using enum contracts::LayoutControlStatus;
    Reconcile(snapshot, now);
    if (!Authorized(principal) ||
        (request.operation == contracts::LayoutControlOperation::BoundaryGesture
             ? principal.role != contracts::WindowRole::LayoutControls
             : principal.role != contracts::WindowRole::TopBar)) {
        return Result(request, snapshot, Rejected, Unauthorized);
    }
    try {
        contracts::ValidateLayoutControl(request);
    } catch (const std::invalid_argument &) {
        return Result(request, snapshot, Rejected, InvalidInput);
    }

    const auto replay =
        std::find_if(journal_.begin(), journal_.end(), [&principal, &request](const auto &entry) {
            return entry.principal == principal && entry.request.request == request.request;
        });
    if (replay != journal_.end()) {
        if (replay->request == request) {
            return replay->result;
        }
        const auto active = sessions_.find(replay->result.session);
        if (active != sessions_.end() && active->second.principal == principal) {
            Cancel(active, InvalidSequence);
        }
        return Result(request, snapshot, Rejected, InvalidSequence);
    }

    auto result = request.phase == contracts::LayoutControlPhase::Begin
                      ? Begin(principal, request, snapshot, now, applier)
                      : Continue(principal, request, snapshot, now, applier);
    if (journal_.size() == MaxJournal) {
        journal_.pop_front();
    }
    journal_.push_back({principal, request, result});
    return result;
}

contracts::LayoutControlResult LayoutControlAuthority::Begin(
    const LayoutControlPrincipal &principal, const contracts::LayoutControlRequest &request,
    const contracts::LayoutSnapshot &snapshot, std::uint64_t now, LayoutControlApplier *applier)
{
    using enum contracts::LayoutControlError;
    using enum contracts::LayoutControlStatus;
    const auto error = ValidateTarget(request, snapshot);
    if (error != None) {
        return Result(request, snapshot, Rejected, error);
    }
    const auto input =
        std::find_if(inputs_.begin(), inputs_.end(), [&principal, &request](const auto &record) {
            return record.principal == principal && record.proof == request.input;
        });
    if (input == inputs_.end() || input->consumed || input->boundary != request.target.boundary) {
        return Result(request, snapshot, Rejected, InvalidInput);
    }
    // Each Down can authorize at most one attempt, including a busy rejection.
    input->consumed = true;
    for (const auto &[id, session] : sessions_) {
        if (session.last.target.workspace == request.target.workspace) {
            return Result(request, snapshot, Rejected, Busy);
        }
    }
    if (next_session_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result(request, snapshot, Rejected, Busy);
    }

    const auto id = next_session_++;
    auto result = Result(request, snapshot, Began);
    result.session = id;
    if (request.operation == contracts::LayoutControlOperation::BoundaryGesture) {
        const auto tracking_error =
            applier ? applier->TrackLayoutIntent(request, result) : Unsupported;
        if (tracking_error != None) {
            result.status = Rejected;
            result.error = tracking_error;
            return result;
        }
    }
    sessions_.emplace(id, Session{principal, request, id, now, now, result.layout_revision});
    return result;
}

contracts::LayoutControlResult LayoutControlAuthority::Continue(
    const LayoutControlPrincipal &principal, const contracts::LayoutControlRequest &request,
    const contracts::LayoutSnapshot &snapshot, std::uint64_t now, LayoutControlApplier *applier)
{
    using enum contracts::LayoutControlError;
    using enum contracts::LayoutControlStatus;
    const auto it = sessions_.find(request.session);
    if (it == sessions_.end()) {
        return Result(request, snapshot, Rejected, UnknownSession);
    }
    auto &session = it->second;
    if (session.principal != principal) {
        return Result(request, snapshot, Rejected, Unauthorized);
    }
    if (request.gesture != session.last.gesture || request.operation != session.last.operation ||
        request.target != session.last.target || request.input != session.last.input) {
        Cancel(it, StaleTarget);
        return Result(request, snapshot, Rejected, StaleTarget);
    }
    if (request.sequence != session.last.sequence + 1) {
        Cancel(it, InvalidSequence);
        return Result(request, snapshot, Rejected, InvalidSequence);
    }

    session.last = request;
    session.updated = now;
    if (request.phase == contracts::LayoutControlPhase::Update) {
        auto result = Result(request, snapshot, Updated);
        if (request.operation == contracts::LayoutControlOperation::BoundaryGesture) {
            const auto error = applier ? applier->TrackLayoutIntent(request, result) : Unsupported;
            // Applying scene geometry can synchronously revoke a surface.
            // Never retain a session iterator across the compositor callback.
            const auto active = sessions_.find(request.session);
            if (error != None || active == sessions_.end()) {
                if (active != sessions_.end()) {
                    Cancel(active, error);
                }
                result.status = Rejected;
                result.error = error != None ? error : InvalidInput;
                return result;
            }
            active->second.layout_revision = result.layout_revision;
        }
        return result;
    }
    sessions_.erase(it);
    if (request.phase == contracts::LayoutControlPhase::Cancel) {
        auto result = Result(request, snapshot, Cancelled);
        if (applier && request.operation == contracts::LayoutControlOperation::BoundaryGesture) {
            applier->TrackLayoutIntent(request, result);
        }
        return result;
    }
    auto result = Result(request, snapshot, Ended);
    if (request.intent == contracts::LayoutControlIntent::None) {
        if (applier && request.operation == contracts::LayoutControlOperation::BoundaryGesture) {
            auto cancel = request;
            cancel.phase = contracts::LayoutControlPhase::Cancel;
            applier->TrackLayoutIntent(cancel, result);
        }
        return result;
    }

    const auto error = applier ? applier->ApplyLayoutIntent(request, result) : Unsupported;
    if (error != None) {
        result.status = Rejected;
        result.error = error;
        result.applied = false;
    }
    return result;
}

void LayoutControlAuthority::Cancel(std::map<std::uint64_t, Session>::iterator it,
                                    contracts::LayoutControlError error)
{
    auto request = it->second.last;
    request.session = it->second.id;
    const auto result =
        Result(request, revisions_, contracts::LayoutControlStatus::Cancelled, error);
    // Bounded by MaxInputs: one live session per proof, drained every event-loop turn.
    if (notifications_.size() < MaxInputs) {
        notifications_.push_back({it->second.principal, result});
    }
    // Replaying a prior accepted request must never resurrect a cancelled session.
    for (auto &entry : journal_) {
        if (entry.principal == it->second.principal && entry.result.session == it->second.id) {
            entry.result.status = contracts::LayoutControlStatus::Cancelled;
            entry.result.error = error;
        }
    }
    sessions_.erase(it);
}

void LayoutControlAuthority::Reconcile(const contracts::LayoutSnapshot &snapshot, std::uint64_t now)
{
    revisions_.session = snapshot.session;
    revisions_.revision = snapshot.revision;
    revisions_.topology_revision = snapshot.topology_revision;
    revisions_.layout_revision = snapshot.layout_revision;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const auto &session = it->second;
        auto current = session.last;
        current.target.layout_revision = session.layout_revision;
        auto error = ValidateTarget(current, snapshot);
        const auto proof =
            std::find_if(inputs_.begin(), inputs_.end(), [&session](const auto &input) {
                return input.principal == session.principal && input.proof == session.last.input;
            });
        if (Elapsed(now, session.started, Lifetime) || Elapsed(now, session.updated, IdleTimeout) ||
            proof == inputs_.end() ||
            (proof->released && Elapsed(now, proof->released, ReleaseGrace))) {
            error = contracts::LayoutControlError::Expired;
        }
        if (error != contracts::LayoutControlError::None) {
            Cancel(it++, error);
        } else {
            ++it;
        }
    }
    std::erase_if(inputs_, [now](const auto &input) {
        return Elapsed(now, input.started, Lifetime) ||
               (input.released && Elapsed(now, input.released, ReleaseGrace));
    });
}

std::vector<LayoutControlDelivery> LayoutControlAuthority::TakeNotifications()
{
    std::vector<LayoutControlDelivery> result;
    result.swap(notifications_);
    return result;
}

bool LayoutControlAuthority::HasPendingInput(contracts::InstanceId instance,
                                             std::uint64_t now) const
{
    for (const auto &input : inputs_) {
        if (input.principal.instance != instance || Elapsed(now, input.started, Lifetime) ||
            (input.released && Elapsed(now, input.released, ReleaseGrace))) {
            continue;
        }
        if (!input.consumed) {
            return true;
        }
        const auto active =
            std::find_if(sessions_.begin(), sessions_.end(), [&input](const auto &entry) {
                return entry.second.principal == input.principal &&
                       entry.second.last.input == input.proof;
            });
        if (active != sessions_.end()) {
            return true;
        }
    }
    return false;
}

} // namespace prism::wm
