#include "prism/runtime/master_load_session.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <poll.h>
#include <set>
#include <unistd.h>

using namespace prism::runtime;

namespace {
struct Package {
    std::filesystem::path root;

    Package()
    {
        char pattern[] = "/tmp/prism-master-graph-XXXXXX";
        const auto directory = mkdtemp(pattern);
        assert(directory);
        root = directory;
    }

    ~Package()
    {
        std::filesystem::remove_all(root);
    }

    void Write(const std::string &file, const std::string &source)
    {
        std::ofstream output(root / file);
        output << source;
        assert(output.good());
    }
};

struct Gate {
    std::mutex mutex;
    std::condition_variable wake;
    std::set<std::string> entered, released;
    std::set<std::string> blocked{"a", "b"};
    unsigned active{}, peak{};

    PreparedComponent Prepare(std::string_view source, ComponentSource metadata,
                              std::stop_token stop)
    {
        const auto name = metadata.component_id;
        {
            std::unique_lock lock(mutex);
            entered.insert(name);
            ++active;
            peak = std::max(peak, active);
            wake.notify_all();
            if (blocked.contains(name)) {
                assert(wake.wait_for(lock, std::chrono::seconds(5), [&] {
                    return released.contains(name) || stop.stop_requested();
                }));
            }
            --active;
        }
        return PrepareComponent(source, std::move(metadata));
    }

    void Await(const std::string &a, const std::string &b = {})
    {
        std::unique_lock lock(mutex);
        assert(wake.wait_for(lock, std::chrono::seconds(3), [&] {
            return entered.contains(a) && (b.empty() || entered.contains(b));
        }));
    }

    bool Entered(const std::string &name)
    {
        std::lock_guard lock(mutex);
        return entered.contains(name);
    }

    void Release(const std::string &name)
    {
        {
            std::lock_guard lock(mutex);
            released.insert(name);
        }
        wake.notify_all();
    }
};

void Pump(MasterLoadSession &session)
{
    pollfd fd{session.Fd(), POLLIN, 0};
    assert(poll(&fd, 1, 3000) == 1);
}

MasterLoadCompletion Await(MasterLoadSession &session)
{
    for (int turn = 0; turn < 300; ++turn) {
        if (auto result = session.TakeCompletion()) {
            return std::move(*result);
        }
        Pump(session);
    }
    std::abort();
}

void ParallelDependencies()
{
    Package package;
    package.Write("master.prism", R"(
Interface(version: 2, layout: "layout.prism") {
    Binding(name: "status", type: "string", initial: "Ready")
    Component(id: "a", source: "a.prism", phase: "critical")
    Component(id: "b", source: "b.prism", phase: "critical")
    Component(id: "c", source: "c.prism", phase: "critical", after: ["a"])
    Component(id: "d", source: "d.prism", phase: "deferred", after: ["c"])
    Component(id: "e", source: "e.prism", phase: "deferred", after: ["d"])
    Component(id: "f", source: "f.prism", phase: "deferred", after: ["e"])
})");
    package.Write("layout.prism", R"(VStack {
Slot(component: "a")
Slot(component: "b")
Slot(component: "c")
Slot(component: "d") { Text("Preparing") }
Slot(component: "e")
Slot(component: "f")
})");
    package.Write("a.prism", "Text($status)");
    package.Write("b.prism", "Text(\"B\")");
    package.Write("c.prism", "Text(\"C\")");
    package.Write("d.prism", "Text(\"D\")");
    package.Write("e.prism", "Text($undeclared)");
    package.Write("f.prism", "Text(\"F\")");
    auto budget = SessionTaskBudget::Create();
    auto scheduler = std::make_shared<TaskScheduler>(budget);
    Gate gate;
    MasterLoadSession session(scheduler, std::bind_front(&Gate::Prepare, &gate));
    const UiLoadId load{71, 1};
    assert(session.Submit({load, package.root / "master.prism", package.root}) ==
           LoadSubmitResult::Accepted);
    Pump(session);
    assert(!session.TakeCompletion());
    gate.Await("a", "b");
    assert(!gate.Entered("c") && !gate.Entered("d"));
    gate.Release("a");
    while (session.Stats().critical_prepared == 0) {
        Pump(session);
        assert(!session.TakeCompletion());
    }
    gate.Await("c");
    assert(!gate.Entered("d"));
    gate.Release("b");
    auto result = Await(session);
    assert(result.load == load && result.prepared && !result.diagnostic);
    assert(result.plan && !result.plan->legacy && result.plan->bindings.size() == 1);
    assert(session.Stats().critical_prepared == 3);
    assert(session.Stats().component_count == 6);
    assert(gate.peak == 2 && scheduler->Stats().workers == 2);
    assert(!session.Stats().deferred_started);
    session.MasterPresented({72, 1});
    assert(!session.Stats().deferred_started);
    session.MasterPresented(load);
    assert(session.Stats().deferred_started);
    while (session.DeferredDiagnostics().empty()) {
        Pump(session);
        assert(!session.TakeCompletion());
    }
    assert(session.Stats().deferred_prepared == 1);
    assert(session.DeferredDiagnostics().front().source.component_id == "e");
    // A failed predecessor is terminal for dependents, without blocking or
    // replacing the already prepared/presented critical UI.
    session.TakeCompletion();
    assert(!gate.Entered("f"));
    assert(session.DeferredDiagnostics().size() == 2);
    auto region = session.TakeRegionCompletion();
    assert(region && region->load == load && region->component == "d" && region->prepared &&
           !region->diagnostic);
    const auto bad = session.TakeRegionCompletion();
    assert(bad && bad->component == "e" && bad->diagnostic && !bad->prepared);
    const auto dependent = session.TakeRegionCompletion();
    assert(dependent && dependent->component == "f" && dependent->diagnostic &&
           !dependent->prepared);
    assert(!session.TakeRegionCompletion());
    const auto held = budget->Stats().retained_bytes;
    assert(held > 0);
    session.Stop();
    assert(budget->Stats().active == 0 && scheduler->Stats().outstanding == 0);
    result.prepared.reset();
    result.plan.reset();
    region.reset();
    assert(budget->Stats().reserved_bytes == 0);
}

void CancelAfterCritical()
{
    Package package;
    package.Write("master.prism", R"(Interface(version: 2, layout: "layout.prism") {
Component(id: "main", source: "main.prism", phase: "critical")
Component(id: "d", source: "d.prism", phase: "deferred")
Component(id: "e", source: "e.prism", phase: "deferred", after: ["d"])
})");
    package.Write("layout.prism", R"(VStack {
Slot(component: "main")
Slot(component: "d")
Slot(component: "e")
})");
    package.Write("main.prism", "Text(\"Main\")");
    package.Write("d.prism", "Text(\"Deferred\")");
    package.Write("e.prism", "Text(\"Dependent\")");

    for (const bool started : {false, true}) {
        auto scheduler = std::make_shared<TaskScheduler>();
        Gate gate;
        gate.blocked = {"d"};
        MasterLoadSession session(scheduler, std::bind_front(&Gate::Prepare, &gate));
        const UiLoadId load{83, 1};
        assert(session.Submit({load, package.root / "master.prism", package.root}) ==
               LoadSubmitResult::Accepted);
        auto result = Await(session);
        assert(result.prepared && !result.diagnostic);
        if (started) {
            session.MasterPresented(load);
            gate.Await("d");
        }

        session.Cancel(load);
        session.MasterPresented(load);
        gate.Release("d");
        assert(!session.TakeCompletion());
        assert(!session.TakeRegionCompletion());
        session.Stop();
        assert(session.Stats().deferred_started == started);
        assert(session.Stats().deferred_prepared == 0);
        assert(!gate.Entered("e"));
        assert(gate.Entered("d") == started);
    }
}

void SourceBoundaries()
{
    Package package;
    package.Write("master.prism", "Text(\"Legacy\")");
    {
        auto small_budget = SessionTaskBudget::Create({2, 16 * 1024 * 1024, 0});
        MasterLoadSession session(std::make_shared<TaskScheduler>(small_budget));
        session.Submit({{5, 1}, package.root / "master.prism", package.root});
        const auto result = Await(session);
        assert(!result.prepared && result.diagnostic);
        assert(result.diagnostic->stage == LoadStage::Semantic);
        assert(result.diagnostic->message.find("budget") != std::string::npos);
        // Scheduling/memory pressure is a runtime preparation failure; it
        // must not misclassify a valid file/package as a Read boundary error.
    }
    auto scheduler = std::make_shared<TaskScheduler>();
    {
        MasterLoadSession session(scheduler);
        assert(session.Submit({{1, 1}, package.root / "master.prism", package.root}) ==
               LoadSubmitResult::Accepted);
        const auto result = Await(session);
        assert(result.prepared && result.plan->legacy && !result.diagnostic);
    }
    package.Write("layout.prism", "Slot(component: \"outside\")");
    package.Write("master.prism", R"(Interface(version: 2, layout: "layout.prism") {
Component(id: "outside", source: "outside.prism", phase: "critical")
})");
    std::filesystem::create_symlink("/etc/passwd", package.root / "outside.prism");
    {
        MasterLoadSession session(scheduler);
        session.Submit({{2, 1}, package.root / "master.prism", package.root});
        const auto result = Await(session);
        assert(!result.prepared && result.diagnostic &&
               result.diagnostic->stage == LoadStage::Read);
        assert(result.diagnostic->message.find("escapes") != std::string::npos);
    }
    std::string plan = "Interface(version: 2, layout: \"layout.prism\") {";
    std::string layout = "VStack {";
    std::string large = "Text(\"Large\")";
    large.resize(kMaxLoadFileBytes, ' ');
    for (int i = 0; i < 8; ++i) {
        const auto id = "unit" + std::to_string(i);
        package.Write(id + ".prism", large);
        plan += "Component(id: \"" + id + "\", source: \"" + id + ".prism\", phase: \"critical\")";
        layout += "Slot(component: \"" + id + "\")";
    }
    package.Write("master.prism", plan + "}");
    package.Write("layout.prism", layout + "}");
    {
        MasterLoadSession session(scheduler);
        session.Submit({{3, 1}, package.root / "master.prism", package.root});
        const auto result = Await(session);
        assert(result.diagnostic && result.diagnostic->stage == LoadStage::Read);
        assert(result.diagnostic->message.find("8 MiB") != std::string::npos);
        assert(session.Stats().critical_prepared == 0);
    }
    {
        MasterLoadSession session(scheduler);
        session.Submit({{4, 1}, package.root / "master.prism", package.root});
        session.Cancel({4, 1});
        const auto result = session.TakeCompletion();
        assert(result && result->diagnostic && result->diagnostic->stage == LoadStage::Cancelled);
    }
}
} // namespace

int main()
{
    ParallelDependencies();
    CancelAfterCritical();
    SourceBoundaries();
}
