#include "prism/runtime/control_value.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
namespace {
void ValidateDomain(const ControlDomain &domain)
{
    if (const auto *number = std::get_if<NumberDomain>(&domain)) {
        if (!std::isfinite(number->minimum) || !std::isfinite(number->maximum) ||
            !std::isfinite(number->step) || number->maximum <= number->minimum ||
            number->step < 0) {
            throw std::invalid_argument("invalid control numeric domain");
        }
    }

    if (const auto *choice = std::get_if<ChoiceDomain>(&domain)) {
        if (choice->keys.empty()) {
            throw std::invalid_argument("empty control choice domain");
        }
        auto keys = choice->keys;
        std::sort(keys.begin(), keys.end());
        if (keys.front().empty() || std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
            throw std::invalid_argument("empty or duplicate control choice key");
        }
    }
}
} // namespace

ControlValueSession::ControlValueSession(ControlDomain domain, ControlValue value,
                                         std::uint64_t revision)
    : domain_(std::move(domain)), revision_(revision)
{
    ValidateDomain(domain_);

    authoritative_ = Normalize(std::move(value));
    presented_ = authoritative_;
}

ControlValue ControlValueSession::Normalize(ControlValue value) const
{
    if (std::holds_alternative<BooleanDomain>(domain_) && std::holds_alternative<bool>(value)) {
        return value;
    }

    if (const auto *domain = std::get_if<NumberDomain>(&domain_)) {
        const auto *number = std::get_if<double>(&value);
        if (!number || !std::isfinite(*number)) {
            throw std::invalid_argument("control requires a finite number");
        }

        long double result = std::clamp(*number, domain->minimum, domain->maximum);
        if (domain->step > 0 && result != domain->minimum && result != domain->maximum) {
            const long double minimum = domain->minimum;
            const long double step = domain->step;
            result = minimum + std::round((result - minimum) / step) * step;
        }
        result = std::clamp(result, static_cast<long double>(domain->minimum),
                            static_cast<long double>(domain->maximum));

        return static_cast<double>(result);
    }

    if (const auto *domain = std::get_if<ChoiceDomain>(&domain_)) {
        const auto *key = std::get_if<std::string>(&value);
        if (key &&
            std::find(domain->keys.begin(), domain->keys.end(), *key) != domain->keys.end()) {
            return value;
        }
    }

    throw std::invalid_argument("control value does not belong to its domain");
}

std::uint64_t ControlValueSession::Begin()
{
    if (Active()) {
        throw std::logic_error("control interaction already active");
    }
    if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("control interaction sequence exhausted");
    }

    presented_ = authoritative_;
    active_ = ++sequence_;
    return active_;
}

bool ControlValueSession::Matches(std::uint64_t interaction) const
{
    return active_ != 0 && interaction == active_;
}

ControlValueEvent ControlValueSession::Event(ValuePhase phase, ControlValue value,
                                             ValueCancelReason reason) const
{
    return {active_, revision_, phase, authoritative_, std::move(value), reason};
}

std::optional<ControlValueEvent> ControlValueSession::Preview(std::uint64_t interaction,
                                                              ControlValue value)
{
    if (!Matches(interaction)) {
        return std::nullopt;
    }

    auto normalized = Normalize(std::move(value));
    if (normalized == presented_) {
        return std::nullopt;
    }

    presented_ = std::move(normalized);
    return Event(ValuePhase::Preview, presented_);
}

std::optional<ControlValueEvent> ControlValueSession::Commit(std::uint64_t interaction,
                                                             ControlValue value)
{
    if (!Matches(interaction)) {
        return std::nullopt;
    }

    auto event = Event(ValuePhase::Commit, Normalize(std::move(value)));
    active_ = 0;
    presented_ = authoritative_;
    return event;
}

std::optional<ControlValueEvent> ControlValueSession::Cancel(std::uint64_t interaction,
                                                             ValueCancelReason reason)
{
    if (!Matches(interaction)) {
        return std::nullopt;
    }
    if (reason == ValueCancelReason::None) {
        throw std::invalid_argument("control cancellation requires a reason");
    }

    auto event = Event(ValuePhase::Cancel, authoritative_, reason);
    active_ = 0;
    presented_ = authoritative_;
    return event;
}

std::optional<ControlValueEvent> ControlValueSession::Synchronize(ControlValue value,
                                                                  std::uint64_t revision)
{
    if (revision < revision_) {
        return std::nullopt;
    }

    auto normalized = Normalize(std::move(value));
    if (revision == revision_) {
        if (normalized != authoritative_) {
            throw std::invalid_argument("control revision reused with another value");
        }
        return std::nullopt;
    }

    auto cancelled = Cancel(active_, ValueCancelReason::Superseded);
    authoritative_ = std::move(normalized);
    presented_ = authoritative_;
    revision_ = revision;
    return cancelled;
}

const ControlValue &ControlValueSession::PresentedValue() const
{
    return presented_;
}

const ControlValue &ControlValueSession::AuthoritativeValue() const
{
    return authoritative_;
}

bool ControlValueSession::Active() const
{
    return active_ != 0;
}

} // namespace prism::runtime
