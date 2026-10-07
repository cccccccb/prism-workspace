#include "prism/runtime/control_value_delivery.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/sdk/module_session.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr UiLoadId Ui{7, 1};

ControlEdit Edit(ValuePhase phase, std::uint64_t interaction = 1, double value = .7,
                 ValueCancelReason reason = ValueCancelReason::None)
{
    return {{2, 1}, "volume", {interaction, 4, phase, .2, value, reason}};
}

struct Harness {
    enum class Behavior {
        None,
        Reenter,
        ReplaceUi,
        ChangeValue,
        Detach,
        ReplaceHandler,
        Close,
        Throw
    };
    ControlValueDelivery delivery;
    UiLoadId ui{Ui};
    std::uint64_t revision{4};
    bool available{true};
    int depth{}, maximum_depth{};
    Behavior behavior{};
    std::vector<ControlEdit> received, replacement;

    Harness()
    {
        delivery.SetHandler(std::bind_front(&Harness::Receive, this));
    }

    ValueCancelReason Validate(UiLoadId load, const ControlEdit &edit) const
    {
        if (load != ui || !available) {
            return ValueCancelReason::Unavailable;
        }
        return edit.event.revision == revision ? ValueCancelReason::None
                                               : ValueCancelReason::Superseded;
    }

    void Drain()
    {
        delivery.Drain(std::bind_front(&Harness::Validate, this));
    }

    void Replacement(const ControlEdit &edit)
    {
        replacement.push_back(edit);
    }

    void Receive(const ControlEdit &edit)
    {
        ++depth;
        maximum_depth = std::max(maximum_depth, depth);
        received.push_back(edit);
        if (edit.event.phase == ValuePhase::Preview) {
            const auto next = std::exchange(behavior, Behavior::None);
            switch (next) {
            case Behavior::Reenter:
                delivery.Enqueue(ui, Edit(ValuePhase::Commit));
                Drain();
                break;
            case Behavior::ReplaceUi:
                ++ui.generation;
                Drain();
                break;
            case Behavior::ChangeValue:
                ++revision;
                Drain();
                break;
            case Behavior::Detach:
                delivery.SetHandler({});
                break;
            case Behavior::ReplaceHandler:
                delivery.SetHandler(std::bind_front(&Harness::Replacement, this));
                delivery.Enqueue(ui, Edit(ValuePhase::Commit, 2));
                Drain();
                break;
            case Behavior::Close:
                available = false;
                Drain();
                break;
            case Behavior::Throw:
                --depth;
                throw std::runtime_error("receiver failure");
            case Behavior::None:
                break;
            }
        }
        --depth;
    }
};

void ReentrantAndAtomic()
{
    Harness h;
    h.behavior = Harness::Behavior::Reenter;
    h.delivery.Enqueue(Ui, Edit(ValuePhase::Preview));
    h.Drain();
    assert(h.maximum_depth == 1 && h.received.size() == 2);
    assert(h.received[1].event.phase == ValuePhase::Commit);
    h.available = false;
    h.Drain();
    assert(h.received.size() == 2); // Already committed; no synthetic cancellation.

    Harness atomic;
    auto edit = Edit(ValuePhase::Commit);
    atomic.delivery.Enqueue(Ui, edit);
    edit.action = "changed after enqueue";
    atomic.Drain();
    assert(atomic.received.size() == 1 && atomic.received[0].action == "volume");
    atomic.delivery.Enqueue({Ui.owner, Ui.generation + 1}, Edit(ValuePhase::Commit));
    atomic.delivery.Enqueue(Ui, Edit(ValuePhase::Cancel, 2, .2, ValueCancelReason::Escape));
    atomic.Drain();
    assert(atomic.received.size() == 1); // No preview was observed for either event.
}

void Invalidation()
{
    for (auto behavior :
         {Harness::Behavior::ReplaceUi, Harness::Behavior::ChangeValue, Harness::Behavior::Close}) {
        Harness h;
        h.behavior = behavior;
        h.delivery.Enqueue(Ui, Edit(ValuePhase::Preview));
        h.delivery.Enqueue(Ui, Edit(ValuePhase::Commit));
        h.Drain();
        assert(h.maximum_depth == 1 && h.received.size() == 2);
        const auto &cancel = h.received.back().event;
        assert(cancel.phase == ValuePhase::Cancel && cancel.value == cancel.before);
        assert(cancel.reason == (behavior == Harness::Behavior::ChangeValue
                                     ? ValueCancelReason::Superseded
                                     : ValueCancelReason::Unavailable));
        h.Drain();
        assert(h.received.size() == 2);
    }

    Harness unobserved;
    unobserved.delivery.Enqueue(Ui, Edit(ValuePhase::Preview));
    ++unobserved.revision;
    unobserved.Drain();
    assert(unobserved.received.empty());
}

void ReceiverLifetime()
{
    Harness detached;
    detached.behavior = Harness::Behavior::Detach;
    detached.delivery.Enqueue(Ui, Edit(ValuePhase::Preview));
    detached.delivery.Enqueue(Ui, Edit(ValuePhase::Commit));
    detached.Drain();
    assert(detached.received.size() == 1);
    detached.available = false;
    detached.Drain();
    assert(detached.received.size() == 1);

    Harness replaced;
    replaced.behavior = Harness::Behavior::ReplaceHandler;
    replaced.delivery.Enqueue(Ui, Edit(ValuePhase::Preview));
    replaced.delivery.Enqueue(Ui, Edit(ValuePhase::Commit));
    replaced.Drain();
    assert(replaced.received.size() == 1 && replaced.replacement.size() == 1);
    assert(replaced.replacement[0].event.interaction == 2);

    Harness failed;
    failed.behavior = Harness::Behavior::Throw;
    failed.delivery.Enqueue(Ui, Edit(ValuePhase::Preview));
    failed.delivery.Enqueue(Ui, Edit(ValuePhase::Commit));
    bool threw = false;
    try {
        failed.Drain();
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    failed.delivery.Enqueue(Ui, Edit(ValuePhase::Commit, 2));
    failed.Drain();
    assert(failed.received.size() == 2 && failed.received.back().event.interaction == 2);
}

void NumericProducerAndCancellation()
{
    Harness h;
    ControlValueSession producer(NumberDomain{0, 1, .1}, .2, 4);
    const auto interaction = producer.Begin();
    const auto preview = producer.Preview(interaction, .68);
    h.delivery.Enqueue(Ui, {{2, 1}, "volume", *preview});
    h.Drain();
    h.delivery.Enqueue(Ui, {{2, 1}, "volume", *preview}); // Defensive duplicate suppression.
    h.Drain();
    assert(h.received.size() == 1);

    auto cancelled = producer.Cancel(interaction, ValueCancelReason::Escape);
    h.delivery.Enqueue(Ui, {{2, 1}, "volume", *cancelled});
    h.delivery.Enqueue(Ui, {{2, 1}, "volume", *cancelled});
    h.Drain();
    assert(h.received.size() == 2 && h.received.back().event.reason == ValueCancelReason::Escape);
    assert(std::get<double>(h.received.back().event.value) == .2);
    assert(!producer.Commit(interaction, .9));

    h.delivery.Enqueue(Ui, Edit(ValuePhase::Preview, 2, .4));
    h.delivery.Enqueue(Ui, Edit(ValuePhase::Preview, 3, .6));
    h.Drain();
    assert(h.received.size() == 5);
    assert(h.received[3].event.phase == ValuePhase::Cancel);
    assert(h.received[3].event.reason == ValueCancelReason::Superseded);
    assert(h.received[4].event.interaction == 3);
}

void LimitsAndValidation()
{
    Harness h;
    bool rejected = false;
    try {
        h.delivery.Enqueue(Ui,
                           Edit(ValuePhase::Preview, 1, std::numeric_limits<double>::quiet_NaN()));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);

    for (std::uint64_t i = 1; i <= 1024; ++i) {
        h.delivery.Enqueue(Ui, Edit(ValuePhase::Commit, i));
    }
    rejected = false;
    try {
        h.delivery.Enqueue(Ui, Edit(ValuePhase::Commit, 1025));
    } catch (const std::length_error &) {
        rejected = true;
    }
    assert(rejected);
    h.Drain();
    assert(h.received.size() == 1024);
}

ShapedText Shape(std::string_view text, double size)
{
    return {{}, text.size() * 7.0, size};
}

struct SceneReceiver {
    Scene &scene;
    sdk::ModuleSession &module;
    int count{};

    ValueCancelReason Validate(UiLoadId ui, const ControlEdit &edit) const
    {
        return ui == Ui ? scene.ControlEditInvalidation(edit) : ValueCancelReason::Unavailable;
    }

    void Receive(const ControlEdit &edit)
    {
        ++count;
        module.ControlValue(edit);
    }
};

void RealCheckbox(const char *module_path)
{
    Scene scene(
        ParseBlueprint(
            R"(Checkbox(checked: $notifications, action: "notify", height: 44) { Visual })"),
        Shape);
    scene.SetViewport({200, 44});
    scene.SetBinding("notifications", false);
    assert(scene.Build({1}));
    sdk::ModuleSession module(module_path, "control.fixture", 1,
                              std::bind_front(&Scene::SetBinding, &scene));
    assert(module.Start());
    // Exercise Scene proposal -> queue -> C module -> authoritative binding.
    auto key = contracts::KeyEvent{};
    key.state = contracts::ButtonState::Pressed;
    key.physical_key = 0x2b;
    scene.HandleInput(key);
    key.physical_key = 0x2c;
    scene.HandleInput(key);
    key.state = contracts::ButtonState::Released;
    const auto result = scene.HandleInput(key);
    assert(result.control_edit);

    SceneReceiver receiver{scene, module};
    ControlValueDelivery delivery;
    delivery.SetHandler(std::bind_front(&SceneReceiver::Receive, &receiver));
    delivery.Enqueue(Ui, *result.control_edit);
    delivery.Drain(std::bind_front(&SceneReceiver::Validate, &receiver));
    assert(receiver.count == 1);
    assert(scene.State(scene.RootId()).selected);
    assert(scene.ControlEditInvalidation(*result.control_edit) == ValueCancelReason::Superseded);

    scene.SetEnabled(scene.RootId(), false);
    assert(scene.ControlEditInvalidation(*result.control_edit) == ValueCancelReason::Unavailable);
    delivery.Enqueue(Ui, *result.control_edit);
    delivery.Drain(std::bind_front(&SceneReceiver::Validate, &receiver));
    assert(receiver.count == 1);
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    ReentrantAndAtomic();
    Invalidation();
    ReceiverLifetime();
    NumericProducerAndCancellation();
    LimitsAndValidation();
    RealCheckbox(argv[1]);
}
