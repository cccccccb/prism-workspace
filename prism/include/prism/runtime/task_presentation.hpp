#pragma once

#include "prism/contracts/types.hpp"
#include "prism/runtime/task_session.hpp"
#include "prism/runtime/ui_load.hpp"

#include <cstdint>
#include <optional>

namespace prism::runtime {

struct TaskPresentationIdentity {
    TaskIdentity task;
    UiLoadId ui;
    std::uint64_t cycle{};

    bool operator==(const TaskPresentationIdentity &) const noexcept = default;
};

enum class TaskPresentationPhase { Opening, Open, Closing, Closed };
enum class TaskPresentationEndpoint { Open, Closed };
enum class TaskPresentationSampleKind { Intermediate, Terminal };
enum class TaskPresentationInterruptReason {
    UiReplaced,
    OwnerRetired,
    FrontendFailed,
    GeometryChanged,
    ThemeChanged,
    ScopeUnavailable,
    Superseded
};

// A descriptor of one owner-local projection. The SDK verifies its Scene and
// actual worker adoption before passing a stamp to this pure lifecycle core.
struct TaskPresentationBinding {
    std::uint64_t modal_token{};
    std::uint64_t modal_epoch{};
    contracts::NodeId root{};
    std::uint64_t input_scene{};
    std::uint64_t input_version{};
    int configure_count{};
    std::uint32_t buffer_width{};
    std::uint32_t buffer_height{};
    double scale{1.0};
    std::uint64_t theme_generation{};

    bool operator==(const TaskPresentationBinding &) const noexcept = default;
};

// Carried by the immutable FramePacket that was actually consumed. Publication
// alone is not adoption, TaskReady, pixel submission or display presentation.
struct TaskPresentationStamp {
    TaskPresentationIdentity identity;
    std::uint64_t projection{};
    TaskPresentationEndpoint endpoint{TaskPresentationEndpoint::Open};
    TaskPresentationBinding binding;
    std::uint64_t frame_sequence{};
    TaskPresentationSampleKind sample_kind{TaskPresentationSampleKind::Terminal};

    bool operator==(const TaskPresentationStamp &) const noexcept = default;
};

struct TaskPresentationState {
    TaskPresentationIdentity identity;
    TaskPresentationPhase phase{TaskPresentationPhase::Opening};
    std::uint64_t projection{};
    std::optional<TaskPresentationBinding> binding;
    std::uint64_t adopted_sequence{};
    std::optional<TaskPresentationInterruptReason> interruption;

    bool operator==(const TaskPresentationState &) const noexcept = default;
};

// Owner-thread presentation adoption policy. It never mutates TaskSession,
// creates a timer, invokes callbacks or retains Scene/render resources. Closing
// binds only an already-retired input scope; the adapter owns that retirement.
class TaskPresentationSession {
public:
    TaskPresentationSession() = default;
    TaskPresentationSession(const TaskPresentationSession &) = delete;
    TaskPresentationSession &operator=(const TaskPresentationSession &) = delete;
    TaskPresentationSession(TaskPresentationSession &&) = delete;
    TaskPresentationSession &operator=(TaskPresentationSession &&) = delete;

    // A new cycle may replace Closing/Closed. Opening/Open remain busy. Cycle
    // IDs and accepted frame sequences never wrap or reuse within this session.
    std::optional<TaskPresentationIdentity> Begin(TaskIdentity, UiLoadId, std::uint64_t modal_token,
                                                  std::uint64_t modal_epoch,
                                                  contracts::NodeId root) noexcept;
    // A refreshed modal scope has a strictly newer epoch. Projection readiness
    // resets, while an already Open presentation does not replay Opening.
    bool Reproject(TaskPresentationIdentity, std::uint64_t modal_token, std::uint64_t modal_epoch,
                   contracts::NodeId root) noexcept;
    bool InvalidateProjection(TaskPresentationIdentity) noexcept;

    // Bindings validate input/geometry before state changes. The first binding
    // uses the current projection; a changed descriptor advances that projection.
    // Same-binding publications extend its sequence interval without losing the
    // last accepted adoption. Intermediate samples precede the first Terminal
    // in that projection and cannot complete a phase. Open accepts only Terminal.
    // The sequence intervals are not transport authorization.
    std::optional<TaskPresentationStamp>
    Publish(TaskPresentationIdentity, const TaskPresentationBinding &, std::uint64_t frame_sequence,
            TaskPresentationSampleKind sample_kind = TaskPresentationSampleKind::Terminal) noexcept;
    bool Adopt(const TaskPresentationStamp &) noexcept;

    // The adapter first retires the scope, advances its epoch and removes task
    // input. A cycle that never adopted opens no visual Closing presentation.
    // Otherwise Closed still requires adoption of a matching endpoint frame.
    // If projection exhaustion rejects this call after scope retirement, the
    // adapter must Interrupt instead; cleanup never needs another projection.
    bool BeginClose(TaskPresentationIdentity, std::uint64_t closed_epoch) noexcept;
    bool Interrupt(TaskPresentationIdentity, TaskPresentationInterruptReason) noexcept;
    std::optional<TaskPresentationState> Current() const noexcept;

private:
    struct Scope {
        std::uint64_t token{};
        std::uint64_t epoch{};
        contracts::NodeId root{};
    };

    bool Matches(TaskPresentationIdentity) const noexcept;
    bool IsLive() const noexcept;
    bool MatchesScope(const TaskPresentationBinding &) const noexcept;
    void ClearProjection() noexcept;

    std::uint64_t last_cycle_{};
    std::uint64_t last_frame_sequence_{};
    std::optional<TaskPresentationState> current_;
    Scope scope_;
    bool ever_adopted_{};
    std::uint64_t first_published_sequence_{};
    std::uint64_t last_published_sequence_{};
    std::uint64_t first_terminal_sequence_{};
};

} // namespace prism::runtime
