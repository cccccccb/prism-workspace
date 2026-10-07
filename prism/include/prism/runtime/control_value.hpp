#pragma once

#include "prism/contracts/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace prism::runtime {

using ControlValue = std::variant<bool, double, std::string>;

struct BooleanDomain {};

struct NumberDomain {
    double minimum{0};
    double maximum{1};
    double step{0}; // Zero selects a continuous range.
};

struct ChoiceDomain {
    std::vector<std::string> keys;
};

using ControlDomain = std::variant<BooleanDomain, NumberDomain, ChoiceDomain>;

enum class ValuePhase { Preview, Commit, Cancel };
enum class ValueCancelReason { None, Escape, Unavailable, FocusLost, Superseded };

struct ControlValueEvent {
    std::uint64_t interaction;
    std::uint64_t revision;
    ValuePhase phase;
    ControlValue before;
    ControlValue value;
    ValueCancelReason reason{ValueCancelReason::None};
};

struct ControlEdit {
    contracts::NodeId node;
    std::string action;
    ControlValueEvent event;
};

// Owner-thread state only. Rendering receives copied values, never this object.
// Commit proposes a value; only Synchronize accepts authoritative business state.
class ControlValueSession {
public:
    ControlValueSession(ControlDomain domain, ControlValue value, std::uint64_t revision = 0);

    std::uint64_t Begin();
    std::optional<ControlValueEvent> Preview(std::uint64_t interaction, ControlValue value);
    std::optional<ControlValueEvent> Commit(std::uint64_t interaction, ControlValue value);
    std::optional<ControlValueEvent> Cancel(std::uint64_t interaction, ValueCancelReason reason);
    std::optional<ControlValueEvent> Synchronize(ControlValue value, std::uint64_t revision);

    const ControlValue &PresentedValue() const;
    const ControlValue &AuthoritativeValue() const;
    bool Active() const;

private:
    ControlValue Normalize(ControlValue value) const;
    bool Matches(std::uint64_t interaction) const;
    ControlValueEvent Event(ValuePhase phase, ControlValue value,
                            ValueCancelReason reason = ValueCancelReason::None) const;

    ControlDomain domain_;
    ControlValue authoritative_;
    ControlValue presented_;
    std::uint64_t revision_{};
    std::uint64_t sequence_{};
    std::uint64_t active_{};
};

} // namespace prism::runtime
