#include "prism/runtime/control_value_delivery.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/sdk/module_session.hpp"

#include <cassert>
#include <cmath>
#include <functional>
#include <limits>

using namespace prism;
using namespace prism::runtime;

namespace {
constexpr auto source = R"(
VStack(spacing: 0) {
    Slider(action: "volume", minimum: 0, maximum: 100, step: 5, value: $volume, enabled: $enabled, height: 44) {
        Visual(sliderPart: "track", height: 4, background: #223246FF)
        Visual(sliderPart: "fill", height: 4, background: #83B9FFFF)
        Visual(sliderPart: "thumb", width: 16, height: 16, background: #FFFFFFFF)
    }
    Button("After", action: "after", height: 44)
}
)";

ShapedText Shape(std::string_view text, double size)
{
    return {{}, text.size() * 7.0, size};
}

struct Fixture {
    Scene scene{ParseBlueprint(source), Shape};
    std::shared_ptr<const InputSnapshot> snapshot;
    contracts::NodeId slider;
    contracts::InputSource pointer{1, 1, 1}, keyboard{1, 2, 1};

    Fixture()
    {
        scene.SetBinding("enabled", true);
        scene.SetBinding("volume", 20.0);
        scene.SetViewport({216, 88});
        assert(scene.Build({1}));
        snapshot = scene.InputGeometry();
        for (const auto &item : snapshot->nodes) {
            if (item.action == "volume") {
                slider = item.id;
            }
        }
        assert(slider);
    }

    void Button(double value, bool down)
    {
        const auto track = snapshot->Find(slider)->slider_track;
        scene.HandleInput(
            contracts::PointerButtonEvent{{1},
                                          {track.x + track.width * value / 100, track.y},
                                          contracts::PointerButton::Primary,
                                          down ? contracts::ButtonState::Pressed
                                               : contracts::ButtonState::Released,
                                          0,
                                          1,
                                          pointer},
            snapshot);
    }

    void Motion(double value)
    {
        const auto track = snapshot->Find(slider)->slider_track;
        scene.HandleInput(
            contracts::PointerMotionEvent{
                {1}, {track.x + track.width * value / 100, track.y}, 1, pointer},
            snapshot);
    }

    void Key(std::uint32_t code, bool down = true, bool repeat = false)
    {
        scene.HandleInput(contracts::KeyEvent{{1},
                                              code,
                                              down ? contracts::ButtonState::Pressed
                                                   : contracts::ButtonState::Released,
                                              repeat,
                                              1,
                                              keyboard,
                                              {}},
                          snapshot);
    }

    ControlEdit Event(ValuePhase phase, double value)
    {
        auto events = scene.TakeControlEvents();
        assert(events.size() == 1);
        assert(events[0].event.phase == phase && std::get<double>(events[0].event.value) == value);
        return events.front();
    }
};

void PointerTransactions()
{
    Fixture f;
    f.Button(61, true);
    const auto preview = f.Event(ValuePhase::Preview, 60);
    assert(std::get<double>(preview.event.before) == 20);
    assert(f.scene.State(f.slider).captured && f.scene.State(f.slider).dragging);
    const auto layouts = f.scene.GetRenderStats().layouts;
    const auto snapshot_version = f.snapshot->version;
    assert(f.scene.Build({1}));
    assert(f.scene.GetRenderStats().layouts == layouts);
    assert(f.scene.InputGeometry()->version == snapshot_version);
    f.Motion(61.2);
    assert(f.scene.TakeControlEvents().empty());
    f.Motion(90);
    f.Event(ValuePhase::Preview, 90);
    f.Button(90, false);
    const auto committed = f.Event(ValuePhase::Commit, 90);
    assert(committed.event.interaction == preview.event.interaction);
    assert(f.scene.IsCurrentControlEdit(committed)); // Business has not accepted yet.
    assert(!f.scene.State(f.slider).captured);
    assert(f.scene.SetBinding("volume", 90.0));
    assert(!f.scene.IsCurrentControlEdit(committed));
    f.Button(90, false);
    assert(f.scene.TakeControlEvents().empty());

    f.Button(90, true);
    assert(f.scene.TakeControlEvents().empty());
    f.Motion(-100);
    f.Event(ValuePhase::Preview, 0);
    f.Motion(200);
    f.Event(ValuePhase::Preview, 100);
    f.Key(0x29);
    const auto cancel = f.Event(ValuePhase::Cancel, 90);
    assert(cancel.event.reason == ValueCancelReason::Escape);
    f.Button(70, false);
    assert(f.scene.TakeControlEvents().empty());
}

void KeyboardTransactions()
{
    Fixture f;
    f.Key(0x2b);
    assert(f.scene.State(f.slider).focusVisible);
    f.Key(0x4f);
    f.Event(ValuePhase::Preview, 25);
    f.Key(0x4f, true, true);
    f.Event(ValuePhase::Preview, 30);
    f.Key(0x4f, false);
    f.Event(ValuePhase::Commit, 30);
    f.Key(0x4f, true, true);
    assert(f.scene.TakeControlEvents().empty());

    f.Key(0x4b); // PageUp = ten steps.
    f.Event(ValuePhase::Preview, 70);
    f.Key(0x29);
    f.Event(ValuePhase::Cancel, 20);
    f.Key(0x4b, false);
    assert(f.scene.TakeControlEvents().empty());
    f.Key(0x4d);
    f.Event(ValuePhase::Preview, 100);
    f.Key(0x4d, false);
    f.Event(ValuePhase::Commit, 100);
    f.Key(0x4a);
    f.Event(ValuePhase::Preview, 0);
    f.Key(0x2b);
    assert(f.Event(ValuePhase::Cancel, 20).event.reason == ValueCancelReason::FocusLost);
}

void InvalidationAndSnapshots()
{
    Fixture f;
    f.Button(70, true);
    f.Event(ValuePhase::Preview, 70);
    f.scene.SetBinding("volume", 40.0);
    assert(f.Event(ValuePhase::Cancel, 20).event.reason == ValueCancelReason::Superseded);
    f.Button(70, false);
    assert(f.scene.TakeControlEvents().empty());
    f.Button(80, true);
    f.Event(ValuePhase::Preview, 80);
    f.scene.SetBinding("enabled", false);
    assert(f.Event(ValuePhase::Cancel, 40).event.reason == ValueCancelReason::Unavailable);
    f.Button(70, true); // Old submitted snapshot cannot restore availability.
    assert(f.scene.TakeControlEvents().empty());
    f.scene.SetBinding("enabled", true);
    f.Button(80, true);
    f.Event(ValuePhase::Preview, 80);
    f.scene.Preflight({{"volume", 25.0}});
    assert(f.Event(ValuePhase::Cancel, 40).event.reason == ValueCancelReason::Superseded);

    f.Button(60, true);
    f.Event(ValuePhase::Preview, 60);
    f.scene.SetViewport({416, 88});
    assert(f.scene.Build({1}));
    auto next = f.scene.InputGeometry();
    f.scene.ApplyInputSnapshot(next);
    assert(f.Event(ValuePhase::Cancel, 25).event.reason == ValueCancelReason::Unavailable);
    f.scene.HandleInput(contracts::PointerButtonEvent{{1},
                                                      {50, 22},
                                                      contracts::PointerButton::Primary,
                                                      contracts::ButtonState::Pressed,
                                                      0,
                                                      1,
                                                      f.pointer},
                        {});
    assert(f.scene.TakeControlEvents().empty());
}

void CaptureLifetime()
{
    Fixture f;
    f.Button(70, true);
    f.Event(ValuePhase::Preview, 70);
    f.scene.HandleInput(contracts::PointerLeaveEvent{{1}, 1, f.pointer}, f.snapshot);
    assert(f.scene.TakeControlEvents().empty());
    f.Button(20, false); // Stale adapter position after surface leave is ignored.
    f.Event(ValuePhase::Commit, 70);

    f.Button(60, true);
    f.Event(ValuePhase::Preview, 60);
    f.scene.HandleInput(contracts::FocusEvent{{1}, false, f.keyboard}, f.snapshot);
    assert(f.Event(ValuePhase::Cancel, 20).event.reason == ValueCancelReason::FocusLost);
    f.Button(60, false);
    assert(f.scene.TakeControlEvents().empty());

    f.Button(80, true);
    f.Event(ValuePhase::Preview, 80);
    f.scene.HandleInput(contracts::PointerCancelEvent{{1}, 1, f.pointer}, f.snapshot);
    assert(f.Event(ValuePhase::Cancel, 20).event.reason == ValueCancelReason::Unavailable);

    f.Button(60, true);
    f.Event(ValuePhase::Preview, 60);
    f.scene.SetProperty(f.slider, DslProperty::Visible, false);
    assert(f.Event(ValuePhase::Cancel, 20).event.reason == ValueCancelReason::Unavailable);
    f.scene.SetProperty(f.slider, DslProperty::Visible, true);
    assert(f.scene.TakeControlEvents().empty());
}

void Validation()
{
    Fixture f;
    assert(!f.scene.AcceptsBinding("volume", -1.0));
    assert(!f.scene.SetBinding("volume", 101.0));
    assert(!f.scene.SetBinding("volume", std::numeric_limits<double>::infinity()));
    assert(!f.scene.SetProperty(f.slider, DslProperty::Minimum, -20.0));
    assert(f.scene.SetBinding("volume", 22.0)); // Off-grid authoritative values are preserved.
    f.Button(70, true);
    assert(std::get<double>(f.Event(ValuePhase::Preview, 70).event.before) == 22);
    f.scene.CancelInput();
    f.Event(ValuePhase::Cancel, 22);

    for (const auto bad :
         {"Slider(minimum: 2, maximum: 1)", "Slider(minimum: $low)",
          "Slider { Visual(sliderPart: \"track\", height: 4) }",
          "Card { Visual(sliderPart: \"thumb\", width: 16, height: 16) }", "Progress(value: 2)"}) {
        bool rejected = false;
        try {
            Scene invalid(ParseBlueprint(bad), Shape);
        } catch (const std::exception &) {
            rejected = true;
        }
        assert(rejected);
    }
}

struct Receiver {
    Scene &scene;
    sdk::ModuleSession &module;
    std::vector<ControlEdit> received;

    void Receive(const ControlEdit &edit)
    {
        received.push_back(edit);
        module.ControlValue(edit);
    }

    ValueCancelReason Validate(UiLoadId, const ControlEdit &edit) const
    {
        return scene.ControlEditInvalidation(edit);
    }
};

void Deliver(Scene &scene, ControlValueDelivery &delivery, Receiver &receiver)
{
    for (auto &edit : scene.TakeControlEvents()) {
        delivery.Enqueue({1, 1}, std::move(edit));
    }
    delivery.Drain(std::bind_front(&Receiver::Validate, &receiver));
}

void Module(const char *path)
{
    Fixture f;
    sdk::ModuleSession module(path, "slider.fixture", 1,
                              std::bind_front(&Scene::SetBinding, &f.scene));
    assert(module.Start());
    Receiver receiver{f.scene, module, {}};
    ControlValueDelivery delivery;
    delivery.SetHandler(std::bind_front(&Receiver::Receive, &receiver));
    f.Button(75, true);
    Deliver(f.scene, delivery, receiver);
    assert(receiver.received.size() == 1);
    assert(f.scene.IsCurrentControlEdit(receiver.received.back()));
    f.Button(75, false);
    Deliver(f.scene, delivery, receiver);
    assert(receiver.received.size() == 2);
    assert(!f.scene.IsCurrentControlEdit(receiver.received.back()));
    f.Button(50, true);
    Deliver(f.scene, delivery, receiver);
    f.Key(0x29);
    Deliver(f.scene, delivery, receiver);
    assert(receiver.received.back().event.phase == ValuePhase::Cancel);
    assert(std::get<double>(receiver.received.back().event.value) == 75);
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    PointerTransactions();
    KeyboardTransactions();
    InvalidationAndSnapshots();
    CaptureLifetime();
    Validation();
    Module(argv[1]);
}
