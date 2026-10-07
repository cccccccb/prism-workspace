#include "fixtures/feedback_fixture.h"
#include "module_feedback_p.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace prism;

namespace {
class Fixture {
public:
    explicit Fixture(const char *path) : handle_(dlopen(path, RTLD_NOW | RTLD_LOCAL))
    {
        assert(handle_);
        host_ = Symbol<decltype(host_)>("feedback_fixture_host");
        create_ = Symbol<decltype(create_)>("feedback_fixture_create_request");
        capabilities_ = Symbol<decltype(capabilities_)>("feedback_fixture_create_capabilities");
        count_ = Symbol<decltype(count_)>("feedback_fixture_count");
        record_ = Symbol<decltype(record_)>("feedback_fixture_record");
        reenter_ = Symbol<decltype(reenter_)>("feedback_fixture_reenter");
        next_ = Symbol<decltype(next_)>("feedback_fixture_next_request");
        destroy_ = Symbol<decltype(destroy_)>("feedback_fixture_destroy_request");
        dismiss_ = Symbol<decltype(dismiss_)>("feedback_fixture_destroy_dismiss");
    }

    ~Fixture()
    {
        assert(dlclose(handle_) == 0);
    }

    const PrismHostApiV1 *Host() const
    {
        return host_();
    }

    std::uint64_t Created() const
    {
        return create_();
    }

    std::uint32_t Capabilities() const
    {
        return capabilities_();
    }

    unsigned Count() const
    {
        return count_();
    }

    const FeedbackFixtureRecord &Record(unsigned index) const
    {
        const auto *record = record_(index);
        assert(record);
        return *record;
    }

    void Reenter() const
    {
        reenter_(1);
    }

    std::uint64_t Next() const
    {
        return next_();
    }

    std::uint64_t Destroyed() const
    {
        return destroy_();
    }

    std::int32_t Dismissed() const
    {
        return dismiss_();
    }

private:
    template <typename Function> Function Symbol(const char *name)
    {
        const auto value = reinterpret_cast<Function>(dlsym(handle_, name));
        assert(value);
        return value;
    }

    void *handle_;
    const PrismHostApiV1 *(*host_)();
    std::uint64_t (*create_)();
    std::uint32_t (*capabilities_)();
    unsigned (*count_)();
    const FeedbackFixtureRecord *(*record_)(unsigned);
    void (*reenter_)(std::uint32_t);
    std::uint64_t (*next_)();
    std::uint64_t (*destroy_)();
    std::int32_t (*dismiss_)();
};

struct Request {
    std::string title{"Saved"}, message{"Document ready\nContinue editing"};
    std::array<std::string, 2> labels{"Open", "Change location"};
    std::array<PrismFeedbackActionV1, 2> actions;
    PrismFeedbackRequestV1 value;

    Request()
    {
        Refresh();
    }

    void Refresh()
    {
        actions = {{{sizeof(PrismFeedbackActionV1), 7, {labels[0].data(), labels[0].size()}},
                    {sizeof(PrismFeedbackActionV1), 9, {labels[1].data(), labels[1].size()}}}};
        value = {sizeof(value),
                 PRISM_FEEDBACK_INFO_V1,
                 {title.data(), title.size()},
                 {message.data(), message.size()},
                 actions.data(),
                 actions.size(),
                 4000};
    }

    std::uint64_t Show(const Fixture &fixture) const
    {
        return fixture.Host()->show_feedback(fixture.Host()->context, &value);
    }

    void Plain()
    {
        value.actions = nullptr;
        value.actions_size = 0;
    }
};

struct Provider {
    sdk::ModuleSession *session{};
    const PrismHostApiV1 *api{};
    Request *borrowed{};
    std::vector<contracts::OwnerFeedbackRequest> attempts;
    std::vector<std::uint64_t> dismissals;
    bool reject{}, throwing{}, throw_dismiss{}, reject_dismiss{}, stop{}, reenter{},
        query_reentry{}, throw_capabilities{};
    std::uint32_t capabilities{~0u};

    bool Binding(std::string_view, runtime::PropertyValue)
    {
        return true;
    }

    bool Stage(const contracts::OwnerFeedbackRequest &request)
    {
        attempts.push_back(request);
        if (borrowed) {
            borrowed->title.assign("Borrowed value replaced");
            borrowed->message.clear();
            borrowed->labels[0].clear();
        }
        if (reenter) {
            Request nested;
            assert(!api->show_feedback(api->context, &nested.value));
            assert(api->dismiss_feedback(api->context, request.request_id) ==
                   PRISM_FEEDBACK_INVALID_V1);
            assert(!session->DeliverFeedbackAction({request.request_id, 7}));
        }
        if (stop) {
            session->StopWork();
        }
        if (throwing) {
            throw std::runtime_error("Provider failed after staging");
        }
        return !reject;
    }

    bool Dismiss(std::uint64_t id)
    {
        dismissals.push_back(id);
        if (reenter) {
            assert(!session->DeliverFeedbackAction({id, 7}));
        }
        if (throw_dismiss) {
            throw std::runtime_error("Dismiss failed");
        }
        return !reject_dismiss;
    }

    std::uint32_t Capabilities()
    {
        if (throw_capabilities) {
            throw std::runtime_error("Capability provider failed");
        }
        if (query_reentry && api) {
            Request nested;
            assert(api->feedback_capabilities(api->context) == 0);
            assert(!api->show_feedback(api->context, &nested.value));
            assert(!session->DeliverFeedbackAction({1, 7}));
        }
        return capabilities;
    }
};

std::unique_ptr<sdk::ModuleSession> Session(const char *path, Provider &provider,
                                            const Fixture &fixture)
{
    sdk::ModuleSessionLimits limits;
    limits.entry_budget = std::chrono::seconds(5);
    auto session = std::make_unique<sdk::ModuleSession>(
        path, "feedback-fixture", 1, std::bind_front(&Provider::Binding, &provider),
        sdk::ModuleSession::LaunchSink{}, sdk::ModuleSession::SubscribeSink{},
        sdk::ModuleSession::ThemeSink{}, sdk::ModuleSession::ColorSchemeSink{}, nullptr, limits,
        std::filesystem::path{}, sdk::ModuleSession::LayoutSubscribeSink{},
        sdk::ModuleSession::ControlSink{}, sdk::ModuleSession::TaskSink{},
        sdk::ModuleSession::TaskCancelSink{}, sdk::ModuleSession::TaskCapabilitySink{},
        std::bind_front(&Provider::Stage, &provider),
        std::bind_front(&Provider::Dismiss, &provider),
        std::bind_front(&Provider::Capabilities, &provider));
    provider.session = session.get();
    assert(session->SupportsFeedback());
    assert(session->Start());
    provider.api = fixture.Host();
    assert(!fixture.Created() && fixture.Capabilities() == PRISM_FEEDBACK_CAP_OWNER_V1);
    return session;
}

void Lifecycle(const char *path)
{
    Fixture fixture(path);
    Provider provider;
    auto session = Session(path, provider, fixture);
    Request request;
    provider.borrowed = &request;
    provider.reenter = true;
    const auto first = request.Show(fixture);
    assert(first && fixture.Count() == 0);
    assert(provider.attempts.back().title == "Saved" &&
           provider.attempts.back().actions[0].label == "Open");
    provider.borrowed = nullptr;
    request = Request();
    request.Refresh();
    const auto second = request.Show(fixture);
    assert(second > first);
    assert(!session->DeliverFeedbackAction({first, 7}));
    assert(!session->DeliverFeedbackAction({second, 0}));
    assert(!session->DeliverFeedbackAction({second, 99}));
    fixture.Reenter();
    assert(session->DeliverFeedbackAction({second, 9}));
    assert(fixture.Count() == 1 && fixture.Record(0).request_id == second &&
           fixture.Record(0).action_id == 9);
    const auto next = fixture.Next();
    assert(next > second);
    assert(!session->DeliverFeedbackAction({second, 9}));
    assert(fixture.Host()->dismiss_feedback(fixture.Host()->context, next) ==
           PRISM_FEEDBACK_DISMISSED_V1);
    assert(!session->DeliverFeedbackAction({next, 7}));
    assert(fixture.Count() == 1);
    assert(fixture.Host()->dismiss_feedback(fixture.Host()->context, next) ==
           PRISM_FEEDBACK_INVALID_V1);
    const auto active = request.Show(fixture);
    assert(active > next);
    session->StopWork();
    assert(!session->SupportsFeedback() && !session->DeliverFeedbackAction({active, 7}));
    assert(!request.Show(fixture));
    assert(fixture.Host()->feedback_capabilities(fixture.Host()->context) == 0);
    assert(fixture.Host()->dismiss_feedback(fixture.Host()->context, active) ==
           PRISM_FEEDBACK_CLOSED_V1);
    session.reset();
    assert(!fixture.Destroyed() && fixture.Dismissed() == PRISM_FEEDBACK_CLOSED_V1);
}

void Invalid(const char *path)
{
    Fixture fixture(path);
    Provider provider;
    auto session = Session(path, provider, fixture);
    Request request;
    const auto active = request.Show(fixture);
    const auto reject = [&] {
        assert(!request.Show(fixture));
        assert(provider.attempts.size() == 1);
    };
    request.value.struct_size = offsetof(PrismFeedbackRequestV1, duration_ms);
    reject();
    request.Refresh();
    request.value.kind = 3;
    reject();
    request.Refresh();
    request.value.title = {nullptr, 1};
    reject();
    request.Refresh();
    request.value.actions = nullptr;
    reject();
    request.Refresh();
    request.value.actions_size = 3;
    reject();
    request.Refresh();
    request.actions[0].struct_size = sizeof(PrismFeedbackActionV1) - 1;
    reject();
    request.Refresh();
    request.actions[1].id = 7;
    reject();
    request.Refresh();
    request.actions[0].id = 0;
    reject();
    request.Refresh();
    request.value.duration_ms = 999;
    reject();
    request.Refresh();
    request.value.kind = PRISM_FEEDBACK_ERROR_V1;
    reject();
    request.Refresh();
    request.title = "bad\n";
    request.Refresh();
    reject();
    request = Request();
    request.Refresh();
    request.message = "bad\t";
    request.Refresh();
    reject();
    request = Request();
    request.Refresh();
    request.labels[0].assign(49, 'x');
    request.Refresh();
    reject();
    assert(session->DeliverFeedbackAction({active, 7}));
}

void Failure(const char *path)
{
    Fixture fixture(path);
    Provider provider;
    auto session = Session(path, provider, fixture);
    Request request;
    const auto active = request.Show(fixture);
    provider.reject = true;
    assert(!request.Show(fixture));
    provider.reject = false;
    provider.throwing = provider.reenter = true;
    assert(!request.Show(fixture));
    assert(provider.dismissals.size() == 1 && provider.dismissals.back() > active);
    provider.throwing = provider.reenter = false;
    provider.query_reentry = true;
    assert(fixture.Host()->feedback_capabilities(fixture.Host()->context) ==
           PRISM_FEEDBACK_CAP_OWNER_V1);
    provider.capabilities = 0;
    assert(!request.Show(fixture));
    provider.capabilities = ~0u;
    provider.throw_capabilities = true;
    assert(fixture.Host()->feedback_capabilities(fixture.Host()->context) == 0);
    assert(!request.Show(fixture));
    provider.throw_capabilities = false;
    provider.throw_dismiss = true;
    assert(fixture.Host()->dismiss_feedback(fixture.Host()->context, active) ==
           PRISM_FEEDBACK_INVALID_V1);
    provider.throw_dismiss = false;
    provider.reject_dismiss = true;
    assert(fixture.Host()->dismiss_feedback(fixture.Host()->context, active) ==
           PRISM_FEEDBACK_INVALID_V1);
    provider.reject_dismiss = false;
    assert(session->DeliverFeedbackAction({active, 7}));
    const auto next = request.Show(fixture);
    assert(next > provider.dismissals.front());
    provider.stop = true;
    assert(!request.Show(fixture));
    assert(!session->DeliverFeedbackAction({next, 7}));
}

struct ThreadAttempt {
    const Fixture &fixture;
    sdk::ModuleSession &session;
    const Request &request;
    std::uint64_t id;

    void Run()
    {
        assert(!request.Show(fixture));
        assert(fixture.Host()->feedback_capabilities(fixture.Host()->context) == 0);
        assert(fixture.Host()->dismiss_feedback(fixture.Host()->context, id) ==
               PRISM_FEEDBACK_WRONG_THREAD_V1);
        assert(!session.SupportsFeedback());
        assert(!session.DeliverFeedbackAction({id, 7}));
    }
};

void ThreadsAndExhaustion(const char *path)
{
    Fixture fixture(path);
    Provider provider;
    auto session = Session(path, provider, fixture);
    Request request;
    const auto id = request.Show(fixture);
    ThreadAttempt attempt{fixture, *session, request, id};
    std::thread worker(&ThreadAttempt::Run, &attempt);
    worker.join();
    session->feedback_->last_request = std::numeric_limits<std::uint64_t>::max();
    assert(!request.Show(fixture));
    assert(session->DeliverFeedbackAction({id, 7}));
}

void ThrowAction(void *, const PrismFeedbackActionEventV1 *)
{
    throw std::runtime_error("Bad module callback");
}

void CallbackFailure(const char *path)
{
    Fixture fixture(path);
    Provider provider;
    auto session = Session(path, provider, fixture);
    Request request;
    const auto id = request.Show(fixture);
    session->module_.api_.on_feedback_action = ThrowAction;
    assert(!session->DeliverFeedbackAction({id, 7}));
    assert(!session->DeliverFeedbackAction({id, 7}));
    assert(!session->feedback_->pending);
    assert(request.Show(fixture) > id);
}

void AbsentAction(const char *path)
{
    Fixture fixture(path);
    launch::AppModule loader(path);
    assert(!loader.Api().on_feedback_action);
    Provider provider;
    auto session = Session(path, provider, fixture);
    Request request;
    assert(!request.Show(fixture));
    request.Plain();
    const auto id = request.Show(fixture);
    assert(id && !session->DeliverFeedbackAction({id, 7}));
    assert(fixture.Host()->dismiss_feedback(fixture.Host()->context, id) ==
           PRISM_FEEDBACK_DISMISSED_V1);
    assert(fixture.Count() == 0);
}

void AbsentProvider(const char *path)
{
    Fixture fixture(path);
    Provider provider;
    sdk::ModuleSessionLimits limits;
    limits.entry_budget = std::chrono::seconds(5);
    sdk::ModuleSession session(path, "feedback-without-provider", 1,
                               std::bind_front(&Provider::Binding, &provider), {}, {}, {}, {},
                               nullptr, limits);
    assert(!session.SupportsFeedback());
    assert(session.Start());
    Request request;
    assert(fixture.Capabilities() == 0);
    assert(!request.Show(fixture));
    assert(!session.DeliverFeedbackAction({1, 7}));
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 5);
    Lifecycle(argv[1]);
    Invalid(argv[1]);
    Failure(argv[1]);
    ThreadsAndExhaustion(argv[1]);
    CallbackFailure(argv[1]);
    AbsentProvider(argv[1]);
    AbsentAction(argv[2]);
    AbsentAction(argv[3]);
    AbsentAction(argv[4]);
}
