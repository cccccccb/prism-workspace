#include "fixtures/close_fixture.h"
#include "prism/sdk/module_session.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <dlfcn.h>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace prism;

namespace {
class Fixture {
public:
    explicit Fixture(const char *path) : handle_(dlopen(path, RTLD_NOW | RTLD_LOCAL))
    {
        assert(handle_);
        host_ = Symbol<decltype(host_)>("close_fixture_host");
        mode_ = Symbol<decltype(mode_)>("close_fixture_mode");
        request_ = Symbol<decltype(request_)>("close_fixture_request");
        calls_ = Symbol<decltype(calls_)>("close_fixture_calls");
        create_ = Symbol<decltype(create_)>("close_fixture_create_completion");
        sync_ = Symbol<decltype(sync_)>("close_fixture_synchronous_completion");
        duplicate_ = Symbol<decltype(duplicate_)>("close_fixture_duplicate_completion");
        destroy_ = Symbol<decltype(destroy_)>("close_fixture_destroy_completion");
    }

    ~Fixture()
    {
        assert(dlclose(handle_) == 0);
    }

    const PrismHostApiV1 *Host() const
    {
        return host_();
    }

    void Mode(std::int32_t decision, std::int32_t synchronous = -1) const
    {
        mode_(decision, synchronous);
    }

    std::uint64_t Request() const
    {
        return request_();
    }

    unsigned Calls() const
    {
        return calls_();
    }

    std::int32_t CreateResult() const
    {
        return create_();
    }

    std::int32_t SyncResult() const
    {
        return sync_();
    }

    std::int32_t DuplicateResult() const
    {
        return duplicate_();
    }

    std::int32_t DestroyResult() const
    {
        return destroy_();
    }

    std::int32_t Complete(std::uint64_t id, std::uint32_t decision) const
    {
        return Host()->complete_close(Host()->context, id, decision);
    }

private:
    template <typename Function> Function Symbol(const char *name)
    {
        auto value = reinterpret_cast<Function>(dlsym(handle_, name));
        assert(value);
        return value;
    }

    void *handle_;
    const PrismHostApiV1 *(*host_)();
    void (*mode_)(std::int32_t, std::int32_t);
    std::uint64_t (*request_)();
    unsigned (*calls_)();
    std::int32_t (*create_)();
    std::int32_t (*sync_)();
    std::int32_t (*duplicate_)();
    std::int32_t (*destroy_)();
};

struct Bindings {
    sdk::ModuleSession *session{};
    bool reenter{}, stop{};
    unsigned inside{};

    bool Set(std::string_view key, runtime::PropertyValue)
    {
        if (key != "insideclose") {
            return true;
        }
        ++inside;
        if (reenter) {
            assert(session);
            assert(!session->RequestClose());
            assert(!session->ConsumeCloseDecision());
        }
        if (stop) {
            session->StopWork();
        }
        return true;
    }
};

struct ThreadAttempt {
    const Fixture &fixture;
    sdk::ModuleSession &session;
    std::uint64_t request;
    std::int32_t result{99};
    bool requested{true};
    std::optional<bool> consumed;

    void Run()
    {
        result = fixture.Complete(request, PRISM_CLOSE_ACCEPT_V1);
        requested = session.RequestClose();
        consumed = session.ConsumeCloseDecision();
    }
};

std::unique_ptr<sdk::ModuleSession> Session(const char *path, Bindings &bindings)
{
    sdk::ModuleSessionLimits limits;
    limits.entry_budget = std::chrono::seconds(5);
    auto result = std::make_unique<sdk::ModuleSession>(
        path, "close-fixture", 1, std::bind_front(&Bindings::Set, &bindings),
        sdk::ModuleSession::LaunchSink{}, sdk::ModuleSession::SubscribeSink{},
        sdk::ModuleSession::ThemeSink{}, sdk::ModuleSession::ColorSchemeSink{}, nullptr, limits);
    bindings.session = result.get();
    assert(result->Start());
    return result;
}

void Deferred(const char *path)
{
    Fixture fixture(path);
    Bindings bindings;
    auto session = Session(path, bindings);
    assert(fixture.CreateResult() == PRISM_CLOSE_INVALID_V1);
    assert(!session->ConsumeCloseDecision());
    assert(fixture.Complete(0, PRISM_CLOSE_ACCEPT_V1) == PRISM_CLOSE_INVALID_V1);
    assert(!session->RequestClose());
    const auto first = fixture.Request();
    assert(first && fixture.Calls() == 1 && bindings.inside == 1);
    assert(!session->RequestClose() && fixture.Calls() == 1);
    assert(fixture.Complete(first + 1, PRISM_CLOSE_ACCEPT_V1) == PRISM_CLOSE_INVALID_V1);
    assert(fixture.Complete(first, PRISM_CLOSE_DEFER_V1) == PRISM_CLOSE_INVALID_V1);
    assert(fixture.Complete(first, 77) == PRISM_CLOSE_INVALID_V1);

    ThreadAttempt attempt{fixture, *session, first};
    std::thread worker(&ThreadAttempt::Run, &attempt);
    worker.join();
    assert(attempt.result == PRISM_CLOSE_WRONG_THREAD_V1 && !attempt.requested &&
           !attempt.consumed);
    assert(fixture.Complete(first, PRISM_CLOSE_REJECT_V1) == PRISM_CLOSE_COMPLETED_V1);
    assert(fixture.Complete(first, PRISM_CLOSE_ACCEPT_V1) == PRISM_CLOSE_INVALID_V1);
    assert(!session->RequestClose() && fixture.Calls() == 1);
    auto decision = session->ConsumeCloseDecision();
    assert(decision && !*decision);
    assert(!session->ConsumeCloseDecision());

    assert(!session->RequestClose());
    const auto second = fixture.Request();
    assert(second > first && fixture.Calls() == 2);
    assert(fixture.Complete(first, PRISM_CLOSE_ACCEPT_V1) == PRISM_CLOSE_INVALID_V1);
    assert(fixture.Complete(second, PRISM_CLOSE_ACCEPT_V1) == PRISM_CLOSE_COMPLETED_V1);
    assert(!session->RequestClose() && fixture.Calls() == 2);
    decision = session->ConsumeCloseDecision();
    assert(decision && *decision);
    assert(!session->ConsumeCloseDecision());
    assert(session->RequestClose() && fixture.Calls() == 2);
    session->StopWork();
    assert(!session->RequestClose());
    assert(fixture.Complete(second, PRISM_CLOSE_ACCEPT_V1) == PRISM_CLOSE_CLOSED_V1);
    session.reset();
    assert(fixture.DestroyResult() == PRISM_CLOSE_CLOSED_V1);
}

void Synchronous(const char *path)
{
    for (const auto desired : {PRISM_CLOSE_REJECT_V1, PRISM_CLOSE_ACCEPT_V1}) {
        Fixture fixture(path);
        Bindings bindings;
        bindings.reenter = true;
        auto session = Session(path, bindings);
        fixture.Mode(PRISM_CLOSE_DEFER_V1, desired);
        assert(!session->RequestClose());
        assert(fixture.SyncResult() == PRISM_CLOSE_COMPLETED_V1);
        assert(fixture.DuplicateResult() == PRISM_CLOSE_INVALID_V1);
        assert(fixture.Calls() == 1 && bindings.inside == 1);
        const auto decision = session->ConsumeCloseDecision();
        assert(decision && *decision == (desired == PRISM_CLOSE_ACCEPT_V1));
    }
    for (const auto direct :
         std::array<std::int32_t, 3>{PRISM_CLOSE_REJECT_V1, PRISM_CLOSE_ACCEPT_V1, 77}) {
        Fixture fixture(path);
        Bindings bindings;
        bindings.reenter = true;
        auto session = Session(path, bindings);
        fixture.Mode(direct, PRISM_CLOSE_ACCEPT_V1);
        assert(session->RequestClose() == (direct == PRISM_CLOSE_ACCEPT_V1));
        assert(fixture.SyncResult() == PRISM_CLOSE_COMPLETED_V1);
        assert(!session->ConsumeCloseDecision());
        assert(fixture.Complete(fixture.Request(), PRISM_CLOSE_ACCEPT_V1) ==
               PRISM_CLOSE_INVALID_V1);
    }
}

void StopAndExhaustion(const char *path)
{
    {
        Fixture fixture(path);
        Bindings bindings;
        bindings.stop = true;
        auto session = Session(path, bindings);
        fixture.Mode(PRISM_CLOSE_ACCEPT_V1);
        assert(!session->RequestClose());
        assert(!session->ConsumeCloseDecision());
        assert(fixture.Complete(fixture.Request(), PRISM_CLOSE_ACCEPT_V1) == PRISM_CLOSE_CLOSED_V1);
    }
    {
        Fixture fixture(path);
        Bindings bindings;
        auto session = Session(path, bindings);
        assert(!session->RequestClose());
        assert(fixture.Complete(fixture.Request(), PRISM_CLOSE_ACCEPT_V1) ==
               PRISM_CLOSE_COMPLETED_V1);
        session->StopWork();
        assert(!session->ConsumeCloseDecision());
        assert(!session->RequestClose());
    }
    {
        Fixture fixture(path);
        Bindings bindings;
        auto session = Session(path, bindings);
        session->next_close_id_ = std::numeric_limits<std::uint64_t>::max();
        fixture.Mode(PRISM_CLOSE_REJECT_V1);
        assert(!session->RequestClose());
        assert(fixture.Request() == std::numeric_limits<std::uint64_t>::max());
        assert(!session->RequestClose() && fixture.Calls() == 1);
    }
}

std::int32_t ThrowClose(void *, const PrismCloseRequestV1 *)
{
    throw std::runtime_error("Invalid module exception across ABI");
}

std::int32_t ThrowLegacyClose(void *)
{
    throw std::runtime_error("Invalid legacy module exception across ABI");
}

void ThrowingCallbacks(const char *path)
{
    Fixture fixture(path);
    Bindings bindings;
    auto session = Session(path, bindings);
    session->module_.api_.on_close_request = ThrowClose;
    assert(!session->RequestClose());
    assert(!session->ConsumeCloseDecision());
    assert(!session->pending_close_);
    session->module_.api_.on_close_request = nullptr;
    session->module_.api_.on_close_requested = ThrowLegacyClose;
    assert(!session->RequestClose());
    assert(!session->ConsumeCloseDecision());
    assert(!session->close_callback_active_);
}

void Prefix(const char *path)
{
    Fixture fixture(path);
    launch::AppModule loader(path);
    assert(!loader.Api().on_close_request);
    Bindings bindings;
    auto session = Session(path, bindings);
    fixture.Mode(0);
    assert(!session->RequestClose() && fixture.Calls() == 1);
    fixture.Mode(-1); // Legacy callback retains its nonzero-means-accept behavior.
    assert(session->RequestClose() && fixture.Calls() == 2);
    assert(!session->ConsumeCloseDecision());
    assert(fixture.Complete(1, PRISM_CLOSE_ACCEPT_V1) == PRISM_CLOSE_INVALID_V1);
}

void NoCloseCallback(const char *path)
{
    Bindings bindings;
    auto session = Session(path, bindings);
    assert(session->RequestClose());
    assert(session->RequestClose());
    assert(!session->ConsumeCloseDecision());
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 5);
    Deferred(argv[1]);
    Synchronous(argv[1]);
    StopAndExhaustion(argv[1]);
    ThrowingCallbacks(argv[1]);
    Prefix(argv[2]);
    Prefix(argv[3]);
    NoCloseCallback(argv[4]);
}
