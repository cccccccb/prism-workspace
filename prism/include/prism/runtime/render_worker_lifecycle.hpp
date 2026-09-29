#pragma once

#include "prism/runtime/terminal_signal.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace prism::runtime {

struct RenderWorkerGeneration {
    std::uint64_t value{};

    bool operator==(const RenderWorkerGeneration &) const = default;
};

enum class RenderWorkerOpenOutcome { Opened, Failed, Cancelled };
enum class RenderWorkerOpenStatus { Started, Opened, Failed, Cancelled, Stale };
enum class RenderWorkerOpenWait { Opened, Failed, Cancelled, TimedOut, Stale };
enum class RenderWorkerCloseWait { Acknowledged, TimedOut, Stale };

// One instance coordinates one render worker incarnation. Open and Close
// results carry the generation returned by BeginOpen; stale completions cannot
// satisfy a newer wait. The worker must call CompleteClose only after its
// Ganesh, EGL and Wayland objects are gone. A close timeout does not grant
// permission to destroy this object, TerminalSignal or the worker's queues.
class RenderWorkerLifecycle {
public:
    explicit RenderWorkerLifecycle(TerminalSignal &terminal) noexcept;

    RenderWorkerLifecycle(const RenderWorkerLifecycle &) = delete;
    RenderWorkerLifecycle &operator=(const RenderWorkerLifecycle &) = delete;

    // A successful result means the Open request has started. This object and
    // its one-shot TerminalSignal cannot be restarted after a Close request.
    std::optional<RenderWorkerGeneration> BeginOpen();

    // Worker-only results. A late Opened result after cancellation is rejected.
    bool CompleteOpen(RenderWorkerGeneration generation, RenderWorkerOpenOutcome outcome);
    bool CompleteClose(RenderWorkerGeneration generation);

    RenderWorkerOpenStatus OpenStatus(RenderWorkerGeneration generation) const;
    RenderWorkerOpenWait WaitOpen(RenderWorkerGeneration generation,
                                  std::chrono::milliseconds timeout);

    // Close bypasses ordinary bounded queues and wakes the worker through
    // TerminalSignal. It also cancels an unfinished Open wait immediately.
    bool RequestClose(RenderWorkerGeneration generation);
    bool CloseRequested(RenderWorkerGeneration generation) const;
    RenderWorkerCloseWait WaitClose(RenderWorkerGeneration generation,
                                    std::chrono::milliseconds timeout);

private:
    bool Matches(RenderWorkerGeneration generation) const noexcept;

    TerminalSignal &terminal_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    RenderWorkerGeneration generation_{};
    RenderWorkerOpenStatus open_status_{RenderWorkerOpenStatus::Stale};
    bool close_requested_{};
    bool close_acknowledged_{};
};

} // namespace prism::runtime
