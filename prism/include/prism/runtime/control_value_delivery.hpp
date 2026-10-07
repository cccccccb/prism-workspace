#pragma once

#include "prism/runtime/control_value.hpp"
#include "prism/runtime/ui_load.hpp"

#include <deque>
#include <functional>
#include <vector>

namespace prism::runtime {

// Owner-thread callback delivery. No Scene pointers, borrowed C values or renderer state.
// Producers must issue monotonically increasing interaction IDs per node/load and one terminal.
class ControlValueDelivery {
public:
    using Handler = std::function<void(const ControlEdit &)>;
    using Validator = std::function<ValueCancelReason(UiLoadId, const ControlEdit &)>;

    // Replacing/detaching a receiver revokes queued work and preview bookkeeping.
    // It never calls the old receiver during teardown.
    void SetHandler(Handler handler);
    void Enqueue(UiLoadId ui, ControlEdit edit);
    // Validator is synchronous and must not mutate delivery state or invoke user callbacks.
    // A handler may enqueue, mutate the UI, detach itself or request a nested Drain.
    void Drain(const Validator &validate);
    void Clear() noexcept;

private:
    struct Pending {
        UiLoadId ui;
        ControlEdit edit;
    };

    static bool SameTarget(const Pending &a, const Pending &b);
    static bool SameInteraction(const Pending &a, const Pending &b);
    void Cancel(std::size_t index, ValueCancelReason reason);
    bool Reconcile(const Validator &validate);
    void Deliver(Pending pending, const Validator &validate);

    Handler handler_;
    std::deque<Pending> pending_;
    std::vector<Pending> previews_;
    bool delivering_{};
};

} // namespace prism::runtime
