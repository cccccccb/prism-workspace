#include "prism/sdk/module_session.hpp"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <sys/resource.h>
#include <thread>

using namespace prism;

namespace {
std::int64_t Microseconds(const timeval &value)
{
    return static_cast<std::int64_t>(value.tv_sec) * 1'000'000 + value.tv_usec;
}

// Observe the existing construction without loading or warming a fixture first.
// The interval includes ModuleSession construction, not just its dlopen call.
class ConstructorObservation {
public:
    ConstructorObservation()
    {
        before_valid_ = getrusage(RUSAGE_THREAD, &before_) == 0;
        started_ = std::chrono::steady_clock::now();
    }

    void Finish()
    {
        const auto finished = std::chrono::steady_clock::now();
        wall_ns_ =
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started_).count();
        after_valid_ = getrusage(RUSAGE_THREAD, &after_) == 0;
    }

    void Report() const
    {
        std::cerr << "constructor_wall_ns=" << wall_ns_;
        if (!before_valid_ || !after_valid_) {
            std::cerr << ", constructor_thread_usage=unavailable\n";
            return;
        }

        std::cerr << ", thread_utime_us="
                  << Microseconds(after_.ru_utime) - Microseconds(before_.ru_utime)
                  << ", thread_stime_us="
                  << Microseconds(after_.ru_stime) - Microseconds(before_.ru_stime)
                  << ", majflt=" << after_.ru_majflt - before_.ru_majflt
                  << ", minflt=" << after_.ru_minflt - before_.ru_minflt
                  << ", inblock=" << after_.ru_inblock - before_.ru_inblock
                  << ", nvcsw=" << after_.ru_nvcsw - before_.ru_nvcsw
                  << ", nivcsw=" << after_.ru_nivcsw - before_.ru_nivcsw << '\n';
    }

private:
    rusage before_{};
    rusage after_{};
    bool before_valid_{};
    bool after_valid_{};
    std::chrono::steady_clock::time_point started_;
    std::int64_t wall_ns_{};
};

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
    ConstructorObservation observation;
    sdk::ModuleSession module(argv[1], "value.fixture", 1,
                              std::bind_front(&Bindings::Set, &bindings));
    observation.Finish();

    const bool started = module.Start();
    if (!started) {
        std::cerr << module.StartDiagnostic() << " (load_ns=" << module.LoadDurationNs()
                  << ", create_ns=" << module.CreateDurationNs() << ")\n";
        observation.Report();
    }
    assert(started);
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
        ConstructorObservation old_observation;
        sdk::ModuleSession old(argv[i], "old.fixture", i,
                               std::bind_front(&Bindings::Set, &bindings));
        old_observation.Finish();

        const bool old_started = old.Start();
        if (!old_started) {
            std::cerr << old.StartDiagnostic() << " (load_ns=" << old.LoadDurationNs()
                      << ", create_ns=" << old.CreateDurationNs() << ")\n";
            old_observation.Report();
        }
        assert(old_started);
        old.ControlValue(edit);
        assert(bindings.values.empty());
    }
}
