#include "prism/runtime/control_value.hpp"

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace prism::runtime;

namespace {
template <typename Function> void Reject(Function function)
{
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

void NumericTransactions()
{
    ControlValueSession slider(NumberDomain{10, 20, 2}, 10.0, 7);
    auto interaction = slider.Begin();
    assert(!slider.Preview(interaction, 10.1));
    auto preview = slider.Preview(interaction, 13.2);
    assert(preview && preview->phase == ValuePhase::Preview);
    assert(preview->interaction == interaction && preview->revision == 7);
    assert(std::get<double>(preview->before) == 10);
    assert(std::get<double>(preview->value) == 14);
    assert(std::get<double>(slider.AuthoritativeValue()) == 10);
    assert(!slider.Preview(interaction, 14.2));

    Reject([&] { slider.Preview(interaction, std::numeric_limits<double>::quiet_NaN()); });
    assert(std::get<double>(slider.PresentedValue()) == 14);
    assert(slider.Active());

    auto commit = slider.Commit(interaction, 999.0);
    assert(commit && commit->phase == ValuePhase::Commit);
    assert(std::get<double>(commit->value) == 20);
    assert(!slider.Active());
    assert(std::get<double>(slider.PresentedValue()) == 10);
    assert(!slider.Commit(interaction, 12.0));
    assert(!slider.Cancel(interaction, ValueCancelReason::Escape));
    assert(!slider.Synchronize(20.0, 8));
    assert(std::get<double>(slider.PresentedValue()) == 20);

    const auto next = slider.Begin();
    assert(next != interaction);
    assert(!slider.Preview(interaction, 12.0));
    assert(!slider.Commit(interaction, 12.0));
    slider.Preview(next, 16.0);
    auto cancel = slider.Cancel(next, ValueCancelReason::Escape);
    assert(cancel && cancel->reason == ValueCancelReason::Escape);
    assert(std::get<double>(cancel->value) == 20);
    assert(std::get<double>(slider.PresentedValue()) == 20);
}

void AuthoritativeUpdates()
{
    ControlValueSession checkbox(BooleanDomain{}, false, 2);
    auto interaction = checkbox.Begin();
    checkbox.Preview(interaction, true);
    assert(!checkbox.Synchronize(false, 1));
    assert(!checkbox.Synchronize(false, 2));
    assert(checkbox.Active() && std::get<bool>(checkbox.PresentedValue()));
    Reject([&] { checkbox.Synchronize(true, 2); });
    Reject([&] { checkbox.Synchronize(1.0, 3); });
    assert(checkbox.Active());

    auto cancel = checkbox.Synchronize(true, 3);
    assert(cancel && cancel->reason == ValueCancelReason::Superseded);
    assert(cancel->revision == 2 && cancel->interaction == interaction);
    assert(!std::get<bool>(cancel->value));
    assert(!checkbox.Active() && std::get<bool>(checkbox.PresentedValue()));
    assert(!checkbox.Commit(interaction, false));

    interaction = checkbox.Begin();
    // Even an equal value at a new revision supersedes the old interaction.
    assert(checkbox.Synchronize(true, 4));
    assert(!checkbox.Active());
    interaction = checkbox.Begin();
    auto commit = checkbox.Commit(interaction, false);
    assert(commit && !std::get<bool>(commit->value));
    // Rejected proposal: the business need not acknowledge the proposed value.
    assert(std::get<bool>(checkbox.PresentedValue()));
}

void ChoicesAndCancellation()
{
    ControlValueSession segment(ChoiceDomain{{"light", "dark"}}, std::string("light"));
    auto interaction = segment.Begin();
    Reject([&] { segment.Commit(interaction, std::string("unknown")); });
    Reject([&] { segment.Commit(interaction, false); });
    Reject([&] { segment.Cancel(interaction, ValueCancelReason::None); });
    assert(segment.Active());

    bool rejected = false;
    try {
        segment.Begin();
    } catch (const std::logic_error &) {
        rejected = true;
    }
    assert(rejected);

    for (auto reason : {ValueCancelReason::Unavailable, ValueCancelReason::FocusLost}) {
        assert(segment.Cancel(interaction, reason));
        interaction = segment.Begin();
    }
    auto commit = segment.Commit(interaction, std::string("dark"));
    assert(commit && std::get<std::string>(commit->value) == "dark");
    segment.Synchronize(std::string("dark"), 1);
    interaction = segment.Begin();
    // A terminal event is required even if the value did not change.
    assert(segment.Commit(interaction, std::string("dark")));
}

void Domains()
{
    Reject([] { ControlValueSession s(NumberDomain{1, 1, 0}, 1.0); });
    Reject([] { ControlValueSession s(NumberDomain{0, 1, -1}, 0.0); });
    Reject([] { ControlValueSession s(NumberDomain{}, true); });
    Reject([] { ControlValueSession s(BooleanDomain{}, 1.0); });
    Reject([] { ControlValueSession s(ChoiceDomain{}, std::string("light")); });
    Reject([] { ControlValueSession s(ChoiceDomain{{"", "light"}}, std::string("light")); });
    Reject([] { ControlValueSession s(ChoiceDomain{{"a", "a"}}, std::string("a")); });
    Reject([] { ControlValueSession s(NumberDomain{}, std::numeric_limits<double>::infinity()); });

    ControlValueSession continuous(NumberDomain{-1, 1, 0}, 0.125);
    assert(std::get<double>(continuous.AuthoritativeValue()) == 0.125);
    auto id = continuous.Begin();
    auto event = continuous.Commit(id, -9.0);
    assert(std::get<double>(event->value) == -1);

    const auto largest = std::numeric_limits<double>::max();
    const auto smallest = std::numeric_limits<double>::denorm_min();
    ControlValueSession extreme(NumberDomain{-largest, largest, smallest}, 0.0);
    assert(std::isfinite(std::get<double>(extreme.AuthoritativeValue())));
    id = extreme.Begin();
    assert(std::isfinite(std::get<double>(extreme.Commit(id, largest / 2)->value)));

    ControlValueSession endpoint(NumberDomain{0, 10, 3}, 10.0);
    assert(std::get<double>(endpoint.AuthoritativeValue()) == 10);
    id = endpoint.Begin();
    assert(std::get<double>(endpoint.Commit(id, 4.5)->value) == 6);
}
} // namespace

int main()
{
    NumericTransactions();
    AuthoritativeUpdates();
    ChoicesAndCancellation();
    Domains();
}
