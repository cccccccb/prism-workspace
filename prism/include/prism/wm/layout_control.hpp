#pragma once

#include "prism/contracts/layout_control.hpp"
#include <deque>
#include <map>
#include <vector>

namespace prism::wm {

// Constructed by the compositor from its registration and mapped native surface.
// Never decoded from the worker's payload.
struct LayoutControlPrincipal {
    contracts::InstanceId instance;
    contracts::RequestId launch_request;
    std::uint32_t pid{};
    contracts::WindowRole role{contracts::WindowRole::Toplevel};
    bool mapped{};
    bool operator==(const LayoutControlPrincipal &) const = default;
};

struct LayoutControlDelivery {
    LayoutControlPrincipal principal;
    contracts::LayoutControlResult result;
};

// Event-loop callbacks after authentication. Tracking returns authoritative
// post-preview revisions; only End can report an applied terminal intent.
class LayoutControlApplier {
public:
    virtual ~LayoutControlApplier() = default;

    virtual contracts::LayoutControlError TrackLayoutIntent(const contracts::LayoutControlRequest &,
                                                            contracts::LayoutControlResult &)
    {
        return contracts::LayoutControlError::Unsupported;
    }

    virtual contracts::LayoutControlError ApplyLayoutIntent(const contracts::LayoutControlRequest &,
                                                            contracts::LayoutControlResult &) = 0;
};

// Event-loop-owned authority. Neither Scene objects nor renderer resources cross here.
class LayoutControlAuthority {
public:
    void RecordInput(const LayoutControlPrincipal &, const contracts::LayoutInputProof &,
                     std::uint64_t now, std::uint64_t boundary = 0, std::uint64_t node = 0);
    void ReleaseInput(contracts::LayoutInputKind, std::int32_t contact, std::uint64_t now);
    void CancelInput(contracts::LayoutInputKind, std::int32_t contact);
    void CancelInstance(contracts::InstanceId, contracts::LayoutControlError);
    void Revoke(contracts::InstanceId);
    void Reset();
    contracts::LayoutControlResult Apply(const LayoutControlPrincipal &,
                                         const contracts::LayoutControlRequest &,
                                         const contracts::LayoutSnapshot &, std::uint64_t now,
                                         LayoutControlApplier *applier = nullptr);
    void Reconcile(const contracts::LayoutSnapshot &, std::uint64_t now);
    bool HasPendingInput(contracts::InstanceId, std::uint64_t now) const;
    std::vector<LayoutControlDelivery> TakeNotifications();

private:
    struct Input {
        LayoutControlPrincipal principal;
        contracts::LayoutInputProof proof;
        std::uint64_t started{}, released{}, boundary{}, node{};
        bool consumed{};
    };

    struct Session {
        LayoutControlPrincipal principal;
        contracts::LayoutControlRequest last;
        std::uint64_t id{}, started{}, updated{}, layout_revision{};
    };

    struct Replay {
        LayoutControlPrincipal principal;
        contracts::LayoutControlRequest request;
        contracts::LayoutControlResult result;
    };

    contracts::LayoutControlResult Begin(const LayoutControlPrincipal &,
                                         const contracts::LayoutControlRequest &,
                                         const contracts::LayoutSnapshot &, std::uint64_t now,
                                         LayoutControlApplier *applier);
    contracts::LayoutControlResult Continue(const LayoutControlPrincipal &,
                                            const contracts::LayoutControlRequest &,
                                            const contracts::LayoutSnapshot &, std::uint64_t now,
                                            LayoutControlApplier *applier);
    void Cancel(std::map<std::uint64_t, Session>::iterator, contracts::LayoutControlError);

    std::deque<Input> inputs_;
    std::map<std::uint64_t, Session> sessions_;
    std::deque<Replay> journal_;
    std::vector<LayoutControlDelivery> notifications_;
    contracts::LayoutSnapshot revisions_;
    std::uint64_t next_session_{1};
};

} // namespace prism::wm
