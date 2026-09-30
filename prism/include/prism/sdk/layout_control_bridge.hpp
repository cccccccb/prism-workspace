#pragma once

#include "prism/contracts/gesture.hpp"
#include "prism/contracts/layout_control.hpp"
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace prism::sdk {

// Owner-thread bridge. A module opts in during a real Begin callback; credentials
// and subsequent motion come exclusively from Scene. One request is in flight
// per gesture, with bounded pending motion and a preserved terminal event.
class LayoutControlBridge {
public:
    using Send = std::function<bool(const contracts::LayoutControlRequest &)>;
    explicit LayoutControlBridge(Send send = {});
    void Observe(const contracts::GestureEvent &);
    void Advance();
    bool Begin(std::uint64_t gesture, contracts::LayoutControlOperation,
               const contracts::LayoutControlTarget &);
    bool EndIntent(std::uint64_t gesture, contracts::LayoutControlIntent);
    bool Cancel(std::uint64_t gesture);
    void Receive(const contracts::LayoutControlResult &);
    void Disconnect();
    std::vector<contracts::LayoutControlResult> TakeResults();
    std::size_t ActiveCount() const;

private:
    struct Control {
        contracts::LayoutControlRequest last;
        contracts::GestureEvent latest;
        std::optional<contracts::GestureEvent> update;
        std::optional<contracts::GestureEvent> terminal;
        contracts::LayoutControlIntent intent{contracts::LayoutControlIntent::None};
        bool waiting{};
    };

    bool Submit(Control &, contracts::LayoutControlPhase, contracts::LogicalPoint);
    void Pump(std::uint64_t gesture);
    void Failed(std::uint64_t gesture);
    Send send_;
    std::map<std::uint64_t, Control> controls_;
    std::optional<contracts::GestureEvent> observed_;
    std::vector<contracts::LayoutControlResult> results_;
    std::uint64_t next_request_{1};
    bool disconnected_{};
};

} // namespace prism::sdk
