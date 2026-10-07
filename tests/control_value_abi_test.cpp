#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <functional>
#include <map>
#include <thread>

using namespace prism;

namespace {
struct Bindings {
    std::map<std::string, runtime::PropertyValue> values;

    bool Set(std::string_view name, runtime::PropertyValue value)
    {
        values.insert_or_assign(std::string(name), std::move(value));
        return true;
    }
};

void Deliver(sdk::ModuleSession &module, const runtime::ControlEdit &edit)
{
    module.ControlValue(edit);
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 4);
    Bindings bindings;
    sdk::ModuleSession module(argv[1], "value.fixture", 1,
                              std::bind_front(&Bindings::Set, &bindings));
    assert(module.Start());
    bindings.values.clear();
    runtime::ControlEdit edit{{3, 2}, "echo", {1, 9, runtime::ValuePhase::Commit, false, true}};
    module.ControlValue(edit);
    assert(!std::get<bool>(bindings.values.at("before")));
    assert(std::get<bool>(bindings.values.at("value")));
    assert(std::get<double>(bindings.values.at("phase")) == PRISM_CONTROL_COMMIT_V1);

    edit.event = {2, 9, runtime::ValuePhase::Preview, 0.25, 0.75};
    module.ControlValue(edit);
    assert(std::get<double>(bindings.values.at("before")) == 0.25);
    assert(std::get<double>(bindings.values.at("value")) == 0.75);
    assert(std::get<double>(bindings.values.at("phase")) == PRISM_CONTROL_PREVIEW_V1);

    edit.event = {3,
                  10,
                  runtime::ValuePhase::Cancel,
                  std::string("light"),
                  std::string("dark"),
                  runtime::ValueCancelReason::Superseded};
    module.ControlValue(edit);
    assert(std::get<std::string>(bindings.values.at("before")) == "light");
    assert(std::get<std::string>(bindings.values.at("value")) == "dark");
    assert(std::get<double>(bindings.values.at("reason")) == PRISM_CONTROL_CANCEL_SUPERSEDED_V1);
    assert(std::get<double>(bindings.values.at("phase")) == PRISM_CONTROL_CANCEL_V1);
    edit.event.value = std::string("changed after callback");
    assert(std::get<std::string>(bindings.values.at("value")) == "dark");

    bindings.values.clear();
    std::thread worker(Deliver, std::ref(module), std::cref(edit));
    worker.join();
    assert(bindings.values.empty());
    module.StopWork();
    module.ControlValue(edit);
    assert(bindings.values.empty());

    for (int i = 2; i < argc; ++i) {
        launch::AppModule library(argv[i]);
        assert(!library.Api().on_control_value);
        sdk::ModuleSession old(argv[i], "old.fixture", i,
                               std::bind_front(&Bindings::Set, &bindings));
        assert(old.Start());
        old.ControlValue(edit);
        assert(bindings.values.empty());
    }
}
