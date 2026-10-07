#include "client_application_p.hpp"

#include <cassert>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>

using namespace prism;

namespace {
runtime::ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * 7.0, font};
}

struct Fixture {
    sdk::ClientApplication app{{.app_id = "close-sdk-fixture", .width = 400, .height = 300}};
    unsigned calls{};
    bool decision{};

    Fixture()
    {
        auto layout = runtime::ParseBlueprint(R"(Card { Button("Task", action:"task") })");
        layout.region = "task";
        layout.region_mounted = true;
        auto &impl = *app.impl_;
        impl.scene = std::make_unique<runtime::Scene>(std::move(layout), Shape);
        impl.scene->SetViewport({400, 300});
        impl.scene->Build(contracts::WindowId{1});
        impl.installed_ui = impl.ui_load.Begin();
        impl.opened_once = true;
        app.OnCloseRequested(std::bind_front(&Fixture::CloseRequested, this));
    }

    bool CloseRequested()
    {
        ++calls;
        return decision;
    }

    void Request()
    {
        app.impl_->HandleWindowEvent(contracts::CloseRequestedEvent{{1}}, {});
    }

    unsigned DrainAccepts()
    {
        unsigned accepts{};
        while (auto command = app.impl_->bridge->render_commands.TryPop()) {
            accepts += std::holds_alternative<runtime::AcceptCloseCommand>(*command);
        }
        return accepts;
    }
};

struct ThreadAttempt {
    sdk::ClientApplication &app;
    bool result{true};

    void Run()
    {
        result = app.AcceptClose();
    }
};

void DeferredClose()
{
    Fixture fixture;
    const auto task = fixture.app.BeginOwnerTask("task");
    assert(task && fixture.app.ActiveOwnerTask());
    fixture.DrainAccepts();
    fixture.Request();
    assert(fixture.calls == 1 && fixture.DrainAccepts() == 0);
    assert(fixture.app.ActiveOwnerTask());
    assert(!fixture.app.impl_->owner_tasks_retired);

    ThreadAttempt attempt{fixture.app};
    std::thread worker(&ThreadAttempt::Run, &attempt);
    worker.join();
    assert(!attempt.result && fixture.app.ActiveOwnerTask());
    assert(fixture.DrainAccepts() == 0);

    assert(fixture.app.AcceptClose());
    assert(!fixture.app.ActiveOwnerTask());
    assert(fixture.app.impl_->owner_tasks_retired);
    assert(!fixture.app.TakeOwnerTaskTerminal());
    assert(!fixture.app.BeginOwnerTask("task"));
    assert(fixture.app.AcceptClose());
    fixture.Request();
    assert(fixture.calls == 1 && fixture.DrainAccepts() == 1);
    fixture.app.Close();
    assert(!fixture.app.AcceptClose());
}

void DirectClose()
{
    Fixture fixture;
    fixture.decision = true;
    fixture.Request();
    fixture.Request();
    assert(fixture.calls == 1 && fixture.DrainAccepts() == 1);
    assert(fixture.app.impl_->owner_tasks_retired);
}

void InvalidAndFullQueue()
{
    sdk::ClientApplication unopened{{.app_id = "unopened"}};
    assert(!unopened.AcceptClose());
    assert(!unopened.impl_->owner_tasks_retired);
    {
        Fixture fixture;
        fixture.app.impl_->failed = true;
        assert(!fixture.app.AcceptClose());
        assert(fixture.DrainAccepts() == 0);
    }
    {
        Fixture fixture;
        auto &impl = *fixture.app.impl_;
        // Exhaust the real bounded command queue. The request revokes input,
        // fails through the independent terminal signal, and never retries.
        for (unsigned i = 0; i < 8192; ++i) {
            assert(impl.bridge->render_commands.TryPush(
                       runtime::RenderCommand(runtime::UiEventsProcessedCommand{i + 1})) ==
                   runtime::QueuePushResult::Accepted);
        }
        assert(!fixture.app.AcceptClose());
        assert(impl.owner_tasks_retired);
        assert(impl.bridge->terminal.Reason() == runtime::TerminalReason::CommandQueueFailure);
        assert(!fixture.app.AcceptClose());
        assert(fixture.DrainAccepts() == 0);
    }
}
} // namespace

int main()
{
    DeferredClose();
    DirectClose();
    InvalidAndFullQueue();
}
