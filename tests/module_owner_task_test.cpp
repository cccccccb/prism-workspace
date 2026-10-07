#include "fixtures/owner_task_fixture.h"
#include "prism/contracts/owner_task.hpp"
#include "prism/launch/module.hpp"
#include "prism/sdk/module_session.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace prism;

namespace {
class Fixture {
public:
    explicit Fixture(const char *path) : handle_(dlopen(path, RTLD_NOW | RTLD_LOCAL))
    {
        assert(handle_);
        host_ = Symbol<decltype(host_)>("owner_task_fixture_host");
        create_request_ = Symbol<decltype(create_request_)>("owner_task_fixture_create_request");
        count_ = Symbol<decltype(count_)>("owner_task_fixture_count");
        record_ = Symbol<decltype(record_)>("owner_task_fixture_record");
        reenter_ = Symbol<decltype(reenter_)>("owner_task_fixture_reenter");
        next_request_ = Symbol<decltype(next_request_)>("owner_task_fixture_next_request");
        errors_ = Symbol<decltype(errors_)>("owner_task_fixture_errors");
    }

    ~Fixture()
    {
        assert(dlclose(handle_) == 0);
    }

    const PrismHostApiV1 *Host() const
    {
        return host_();
    }

    std::uint64_t CreateRequest() const
    {
        return create_request_();
    }

    std::size_t Count() const
    {
        return count_();
    }

    const OwnerTaskFixtureRecord &Record(std::size_t at) const
    {
        const auto *record = record_(at);
        assert(record);
        return *record;
    }

    void Reenter() const
    {
        reenter_(1);
    }

    std::uint64_t NextRequest() const
    {
        return next_request_();
    }

    unsigned Errors() const
    {
        return errors_();
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
    std::uint64_t (*create_request_)();
    std::size_t (*count_)();
    const OwnerTaskFixtureRecord *(*record_)(std::size_t);
    void (*reenter_)(std::uint32_t);
    std::uint64_t (*next_request_)();
    unsigned (*errors_)();
};

struct BorrowedRequest {
    std::string title{"Keep your changes?"};
    std::string message{"Choose an action.\nWork remains in this window."};
    std::array<std::string, 2> labels{"Save", "Discard"};
    std::array<PrismTaskChoiceV1, 2> choices{};
    PrismTaskRequestV1 value{};

    BorrowedRequest()
    {
        Refresh();
    }

    void Refresh()
    {
        choices = {{{sizeof(PrismTaskChoiceV1),
                     11,
                     {labels[0].data(), labels[0].size()},
                     PRISM_TASK_CHOICE_PRIMARY_V1},
                    {sizeof(PrismTaskChoiceV1),
                     22,
                     {labels[1].data(), labels[1].size()},
                     PRISM_TASK_CHOICE_DESTRUCTIVE_V1}}};
        value = {sizeof(value),
                 PRISM_TASK_CONFIRMATION_V1,
                 {title.data(), title.size()},
                 {message.data(), message.size()},
                 choices.data(),
                 choices.size(),
                 nullptr};
    }
};

struct BorrowedFileRequest {
    std::string title{"Choose a file"};
    std::string message{"Continue in this window."};
    std::string directory{"/home/ss/Documents/../Documents"};
    std::string name;
    std::vector<std::string> extensions{".md", ".tar.gz"};
    std::vector<PrismTaskStringViewV1> extension_views;
    std::uint32_t kind{PRISM_TASK_OPEN_FILE_V1};
    PrismFileTaskOptionsV1 options{};
    PrismTaskRequestV1 value{};

    BorrowedFileRequest()
    {
        Refresh();
    }

    void Refresh()
    {
        extension_views.clear();
        for (const auto &extension : extensions) {
            extension_views.push_back({extension.data(), extension.size()});
        }
        options = {sizeof(options),
                   {directory.data(), directory.size()},
                   {name.data(), name.size()},
                   extension_views.data(),
                   extension_views.size(),
                   PRISM_FILE_TASK_SHOW_HIDDEN_V1};
        value = {sizeof(value),
                 kind,
                 {title.data(), title.size()},
                 {message.data(), message.size()},
                 nullptr,
                 0,
                 &options};
    }
};

contracts::OwnerTaskResult Success(std::uint64_t id, std::uint32_t choice = 11)
{
    contracts::OwnerTaskResult result;
    result.request_id = id;
    result.choice_id = choice;
    return result;
}

contracts::OwnerTaskResult Cancelled(std::uint64_t id)
{
    contracts::OwnerTaskResult result;
    result.request_id = id;
    result.outcome = contracts::OwnerTaskOutcome::Cancelled;
    result.cancel_reason = contracts::OwnerTaskCancelReason::User;
    return result;
}

contracts::OwnerTaskResult FileSuccess(std::uint64_t id, std::string path, bool overwrite = false)
{
    contracts::OwnerTaskResult result;
    result.request_id = id;
    result.file_path = std::move(path);
    result.overwrite_approved = overwrite;
    return result;
}

struct WrongThreadCall {
    const PrismHostApiV1 *host;
    const PrismTaskRequestV1 *request;
    sdk::ModuleSession *session;
    contracts::OwnerTaskResult result;
    std::uint32_t capabilities{99};
    std::uint64_t requested{99};
    std::int32_t cancelled{99};
    bool delivered{true};

    void operator()()
    {
        capabilities = host->task_capabilities(host->context);
        requested = host->request_task(host->context, request);
        cancelled = host->cancel_task(host->context, result.request_id);
        delivered = session->DeliverTaskResult(result);
        assert(!session->SupportsOwnerTasks());
    }
};

struct Sinks {
    sdk::ModuleSession *session{};
    const PrismHostApiV1 *host{};
    const PrismTaskRequestV1 *nested_request{};
    contracts::OwnerTaskResult *mutate_result{};
    std::thread::id owner{std::this_thread::get_id()};
    std::vector<contracts::OwnerTaskRequest> requests;
    std::vector<std::uint64_t> cancellations;
    std::uint32_t capabilities{contracts::kOwnerTaskConfirmationCapability};
    unsigned capability_calls{}, completions{};
    bool accept{true}, cancel_accept{true}, throw_submit{}, throw_cancel{}, throw_capabilities{};
    bool stop_submit{}, stop_cancel{}, stop_capabilities{};
    bool attempt_inline_result{}, attempt_nested_submit{}, inline_result_accepted{};
    bool attempt_capability_submit{};
    std::uint64_t nested_id{};

    bool Binding(std::string_view key, runtime::PropertyValue value)
    {
        assert(std::this_thread::get_id() == owner && key == "completed");
        assert(std::get<double>(value) == completions + 1);
        ++completions;
        if (mutate_result) {
            *mutate_result = {};
        }
        return true;
    }

    bool Submit(const contracts::OwnerTaskRequest &request)
    {
        assert(std::this_thread::get_id() == owner);
        requests.push_back(request);
        if (attempt_inline_result) {
            inline_result_accepted = session->DeliverTaskResult(Success(request.request_id));
        }
        if (attempt_nested_submit) {
            nested_id = host->request_task(host->context, nested_request);
        }
        if (stop_submit) {
            session->StopWork();
        }
        if (throw_submit) {
            throw std::runtime_error("Fixture provider staging failed");
        }
        return accept;
    }

    bool Cancel(std::uint64_t request)
    {
        assert(std::this_thread::get_id() == owner);
        cancellations.push_back(request);
        if (attempt_inline_result) {
            inline_result_accepted = session->DeliverTaskResult(Cancelled(request));
        }
        if (stop_cancel) {
            session->StopWork();
        }
        if (throw_cancel) {
            throw std::runtime_error("Fixture provider cancellation failed");
        }
        return cancel_accept;
    }

    std::uint32_t Capabilities()
    {
        assert(std::this_thread::get_id() == owner);
        ++capability_calls;
        if (attempt_capability_submit) {
            nested_id = host->request_task(host->context, nested_request);
        }
        if (stop_capabilities) {
            session->StopWork();
        }
        if (throw_capabilities) {
            throw std::runtime_error("Fixture provider capability query failed");
        }
        return capabilities;
    }
};

std::unique_ptr<sdk::ModuleSession> Open(const char *path, Sinks &sinks, unsigned omitted = 0)
{
    sdk::ModuleSession::TaskSink submit;
    sdk::ModuleSession::TaskCancelSink cancel;
    sdk::ModuleSession::TaskCapabilitySink capabilities;
    if (omitted != 1) {
        submit = std::bind_front(&Sinks::Submit, &sinks);
    }
    if (omitted != 2) {
        cancel = std::bind_front(&Sinks::Cancel, &sinks);
    }
    if (omitted != 3) {
        capabilities = std::bind_front(&Sinks::Capabilities, &sinks);
    }
    auto session = std::make_unique<sdk::ModuleSession>(
        path, "owner-task-fixture", 31, std::bind_front(&Sinks::Binding, &sinks),
        sdk::ModuleSession::LaunchSink{}, sdk::ModuleSession::SubscribeSink{},
        sdk::ModuleSession::ThemeSink{}, sdk::ModuleSession::ColorSchemeSink{},
        std::shared_ptr<runtime::TaskScheduler>{}, sdk::ModuleSessionLimits{},
        std::filesystem::path{}, sdk::ModuleSession::LayoutSubscribeSink{},
        sdk::ModuleSession::ControlSink{}, std::move(submit), std::move(cancel),
        std::move(capabilities));
    sinks.session = session.get();
    assert(session->Start());
    return session;
}

std::uint64_t Submit(Fixture &fixture, const PrismTaskRequestV1 &request)
{
    const auto *host = fixture.Host();
    return host->request_task(host->context, &request);
}

void CheckAbiPrefixes(const char *full, const char *exact, const char *partial)
{
    launch::AppModule current(full);
    assert(current.Api().on_task_completed);
    for (const auto *path : {exact, partial}) {
        auto *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        assert(handle);
        auto host =
            reinterpret_cast<const PrismHostApiV1 *(*)()>(dlsym(handle, "owner_task_fixture_host"));
        assert(host);
        {
            launch::AppModule old(path);
            assert(old.Api().create && old.Api().destroy && !old.Api().on_task_completed);
            Sinks sinks;
            auto session = Open(path, sinks);
            assert(!session->SupportsOwnerTasks());
            const auto *api = host();
            assert(api && api->struct_size == sizeof(*api));
            assert(api->task_capabilities(api->context) == 0);
            BorrowedRequest request;
            assert(api->request_task(api->context, &request.value) == 0);
            assert(api->cancel_task(api->context, 1) == -1);
            assert(!session->DeliverTaskResult(Success(1)));
            assert(sinks.requests.empty() && sinks.cancellations.empty() &&
                   sinks.capability_calls == 0);
        }
        assert(dlclose(handle) == 0);
    }
}

struct OriginalTaskRequest {
    std::uint32_t struct_size;
    std::uint32_t kind;
    PrismTaskStringViewV1 title;
    PrismTaskStringViewV1 message;
    const PrismTaskChoiceV1 *choices;
    std::size_t choices_size;
};

static_assert(sizeof(OriginalTaskRequest) == offsetof(PrismTaskRequestV1, file));

class GuardedRequest {
public:
    GuardedRequest(const BorrowedRequest &request, bool partial)
    {
        const auto page = sysconf(_SC_PAGESIZE);
        assert(page > 0);
        page_ = static_cast<std::size_t>(page);
        mapping_ =
            mmap(nullptr, page_ * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(mapping_ != MAP_FAILED);
        assert(mprotect(static_cast<char *>(mapping_) + page_, page_, PROT_NONE) == 0);

        prefix_ = reinterpret_cast<OriginalTaskRequest *>(static_cast<char *>(mapping_) + page_ -
                                                          sizeof(OriginalTaskRequest));
        const OriginalTaskRequest prefix{
            static_cast<std::uint32_t>(partial ? sizeof(PrismTaskRequestV1) - 1
                                               : sizeof(OriginalTaskRequest)),
            request.value.kind,
            request.value.title,
            request.value.message,
            request.value.choices,
            request.value.choices_size};
        std::memcpy(prefix_, &prefix, sizeof(prefix));
    }

    ~GuardedRequest()
    {
        assert(munmap(mapping_, page_ * 2) == 0);
    }

    const PrismTaskRequestV1 *Value() const
    {
        return reinterpret_cast<const PrismTaskRequestV1 *>(prefix_);
    }

    void FileKind()
    {
        prefix_->kind = PRISM_TASK_OPEN_FILE_V1;
        prefix_->choices = nullptr;
        prefix_->choices_size = 0;
    }

private:
    void *mapping_{};
    std::size_t page_{};
    OriginalTaskRequest *prefix_{};
};

void CheckGuardedRequestPrefixes(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    sinks.capabilities = contracts::kOwnerTaskCapabilities;
    auto session = Open(path, sinks);
    BorrowedRequest request;
    const auto *host = fixture.Host();
    for (const auto partial : {false, true}) {
        GuardedRequest guarded(request, partial);
        const auto id = host->request_task(host->context, guarded.Value());
        assert(id && sinks.requests.back().kind == contracts::OwnerTaskKind::Confirmation &&
               !sinks.requests.back().file);
        assert(session->DeliverTaskResult(Success(id)));

        guarded.FileKind();
        const auto count = sinks.requests.size();
        assert(!host->request_task(host->context, guarded.Value()));
        assert(sinks.requests.size() == count);
    }
    assert(fixture.Count() == 2 && fixture.Errors() == 0);
}

void CheckCopiedInputAndSinglePending(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    auto session = Open(path, sinks);
    assert(session->SupportsOwnerTasks() && fixture.CreateRequest() == 0);
    assert(sinks.requests.empty()); // create cannot stage a request.
    assert(fixture.Host()->task_capabilities(fixture.Host()->context) == 1);

    BorrowedRequest borrowed;
    const auto id = Submit(fixture, borrowed.value);
    assert(id == 1 && sinks.requests.size() == 1 && fixture.Count() == 0);
    const auto expected = sinks.requests.front();
    assert(expected.title == borrowed.title && expected.message == borrowed.message);
    assert(expected.choices.size() == 2 && expected.choices[0].label == "Save" &&
           expected.choices[1].label == "Discard");

    std::fill(borrowed.title.begin(), borrowed.title.end(), 'x');
    std::fill(borrowed.message.begin(), borrowed.message.end(), 'x');
    for (auto &label : borrowed.labels) {
        std::fill(label.begin(), label.end(), 'x');
    }
    borrowed.choices[0].id = 99;
    borrowed.choices[1].id = 100;
    assert(sinks.requests.front() == expected);
    assert(!Submit(fixture, borrowed.value));
    assert(!session->DeliverTaskResult(Success(id, 99)));
    assert(sinks.requests.size() == 1 && fixture.Count() == 0);
    assert(session->DeliverTaskResult(Success(id, 11)));
    assert(fixture.Count() == 1 && fixture.Record(0).request_id == id &&
           fixture.Record(0).choice_id == 11);
    assert(!session->DeliverTaskResult(Success(id, 11)));
    assert(!fixture.Host()->request_task(nullptr, &borrowed.value));
    assert(!fixture.Host()->task_capabilities(nullptr));
    assert(fixture.Host()->cancel_task(nullptr, id) == -1);

    BorrowedRequest next;
    next.message.clear();
    next.Refresh();
    next.value.message = {nullptr, 0};
    next.value.choices_size = 1;
    const auto second = Submit(fixture, next.value);
    assert(second > id && sinks.requests.back().message.empty());
    assert(session->DeliverTaskResult(Success(second)));
    assert(fixture.Count() == 2 && fixture.Errors() == 0);
}

void RejectRequest(Fixture &fixture, const PrismTaskRequestV1 *request, const Sinks &sinks)
{
    const auto previous = sinks.requests.size();
    const auto *host = fixture.Host();
    assert(!host->request_task(host->context, request));
    assert(sinks.requests.size() == previous && fixture.Count() == 0);
}

void CheckRequestValidation(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    auto session = Open(path, sinks);
    BorrowedRequest request;
    RejectRequest(fixture, nullptr, sinks);
    request.value.struct_size =
        offsetof(PrismTaskRequestV1, choices_size) + sizeof(request.value.choices_size) - 1;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.value.kind = 99;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.value.choices = nullptr;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.value.choices_size = 0;
    RejectRequest(fixture, &request.value, sinks);
    request.value.choices_size = 3;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.choices[0].struct_size = sizeof(PrismTaskChoiceV1) - 1;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.choices[0].id = 0;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.choices[1].id = request.choices[0].id;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.choices[1].role = PRISM_TASK_CHOICE_PRIMARY_V1;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.choices[0].role = PRISM_TASK_CHOICE_SECONDARY_V1;
    request.choices[1].role = PRISM_TASK_CHOICE_SECONDARY_V1;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.choices[0].role = 99;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.value.title = {nullptr, 1};
    RejectRequest(fixture, &request.value, sinks);
    request.value.title = {nullptr, 0};
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.value.message = {nullptr, 1};
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.choices[0].label = {nullptr, 1};
    RejectRequest(fixture, &request.value, sinks);
    request.choices[0].label = {nullptr, 0};
    RejectRequest(fixture, &request.value, sinks);

    for (const auto &invalid :
         {std::string("Title\n"), std::string("Title\t"), std::string("A\0B", 3),
          std::string("\xC0\xAF", 2), std::string("\xED\xA0\x80", 3),
          std::string("\xF4\x90\x80\x80", 4), std::string("\xE2\x80\xA8", 3),
          std::string(contracts::kMaxOwnerTaskTitleBytes + 1, 't')}) {
        request.title = invalid;
        request.Refresh();
        RejectRequest(fixture, &request.value, sinks);
    }
    request.title = "Valid title";
    for (const auto &invalid : {std::string("A\0B", 3), std::string("A\rB"), std::string("\x80", 1),
                                std::string(contracts::kMaxOwnerTaskMessageBytes + 1, 'm')}) {
        request.message = invalid;
        request.Refresh();
        RejectRequest(fixture, &request.value, sinks);
    }
    request.message = "Valid\nmessage\ttext";
    for (const auto &invalid :
         {std::string("Save\n"), std::string("Save\t"), std::string("\xC2\x85", 2),
          std::string(contracts::kMaxOwnerTaskChoiceLabelBytes + 1, 'c')}) {
        request.labels[0] = invalid;
        request.Refresh();
        RejectRequest(fixture, &request.value, sinks);
    }

    // Exact maxima and non-NUL-terminated slices are accepted. The Host must
    // validate byte slices, not call strlen on their source buffers.
    request.title.assign(contracts::kMaxOwnerTaskTitleBytes, 't');
    request.message.assign(contracts::kMaxOwnerTaskMessageBytes, 'm');
    request.labels[0].assign(contracts::kMaxOwnerTaskChoiceLabelBytes, 'a');
    request.labels[1].assign(contracts::kMaxOwnerTaskChoiceLabelBytes, 'b');
    request.title += "outside slice";
    request.message += "outside slice";
    request.labels[0] += "outside slice";
    request.labels[1] += "outside slice";
    request.Refresh();
    request.value.title.size = contracts::kMaxOwnerTaskTitleBytes;
    request.value.message.size = contracts::kMaxOwnerTaskMessageBytes;
    request.choices[0].label.size = contracts::kMaxOwnerTaskChoiceLabelBytes;
    request.choices[1].label.size = contracts::kMaxOwnerTaskChoiceLabelBytes;
    request.value.struct_size += 16;
    request.choices[0].struct_size += 8;
    const auto id = Submit(fixture, request.value);
    assert(id == 1 && sinks.requests.size() == 1);
    const auto &copied = sinks.requests.front();
    assert(copied.title == std::string(contracts::kMaxOwnerTaskTitleBytes, 't'));
    assert(copied.message == std::string(contracts::kMaxOwnerTaskMessageBytes, 'm'));
    assert(copied.choices[0].label == std::string(contracts::kMaxOwnerTaskChoiceLabelBytes, 'a'));
    assert(copied.choices[1].label == std::string(contracts::kMaxOwnerTaskChoiceLabelBytes, 'b'));
    assert(session->DeliverTaskResult(Success(id)));
    assert(fixture.Count() == 1 && fixture.Errors() == 0);
}

void CheckResultsAndOwnedProjection(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    auto session = Open(path, sinks);
    BorrowedRequest request;
    const auto id = Submit(fixture, request.value);
    assert(id);
    auto invalid = Success(id);
    invalid.request_id = id + 1;
    assert(!session->DeliverTaskResult(invalid));
    invalid = Success(id, 0);
    assert(!session->DeliverTaskResult(invalid));
    invalid = Success(id, 99);
    assert(!session->DeliverTaskResult(invalid));
    invalid = Success(id);
    invalid.outcome = static_cast<contracts::OwnerTaskOutcome>(99);
    assert(!session->DeliverTaskResult(invalid));
    invalid = Success(id);
    invalid.cancel_reason = contracts::OwnerTaskCancelReason::User;
    assert(!session->DeliverTaskResult(invalid));
    invalid = Success(id);
    invalid.failure_code = contracts::OwnerTaskFailureCode::OperationFailed;
    assert(!session->DeliverTaskResult(invalid));
    invalid = Success(id);
    invalid.diagnostic = "Success cannot carry failure text";
    assert(!session->DeliverTaskResult(invalid));
    invalid = Success(id);
    invalid.file_path = "/home/ss/Documents/notes.md";
    assert(!session->DeliverTaskResult(invalid));
    invalid = Success(id);
    invalid.overwrite_approved = true;
    assert(!session->DeliverTaskResult(invalid));
    invalid = Cancelled(id);
    invalid.cancel_reason = contracts::OwnerTaskCancelReason::None;
    assert(!session->DeliverTaskResult(invalid));
    invalid.cancel_reason = static_cast<contracts::OwnerTaskCancelReason>(99);
    assert(!session->DeliverTaskResult(invalid));
    invalid = Cancelled(id);
    invalid.choice_id = 11;
    assert(!session->DeliverTaskResult(invalid));
    invalid = Cancelled(id);
    invalid.failure_code = contracts::OwnerTaskFailureCode::ProviderUnavailable;
    assert(!session->DeliverTaskResult(invalid));
    invalid = Cancelled(id);
    invalid.diagnostic = "Cancelled cannot carry a failure diagnostic";
    assert(!session->DeliverTaskResult(invalid));

    contracts::OwnerTaskResult failed;
    failed.request_id = id;
    failed.outcome = contracts::OwnerTaskOutcome::Failed;
    assert(!session->DeliverTaskResult(failed));
    failed.failure_code = static_cast<contracts::OwnerTaskFailureCode>(99);
    assert(!session->DeliverTaskResult(failed));
    failed.failure_code = contracts::OwnerTaskFailureCode::OperationFailed;
    failed.choice_id = 11;
    assert(!session->DeliverTaskResult(failed));
    failed.choice_id = 0;
    failed.cancel_reason = contracts::OwnerTaskCancelReason::User;
    assert(!session->DeliverTaskResult(failed));
    failed.cancel_reason = contracts::OwnerTaskCancelReason::None;
    failed.diagnostic.assign(contracts::kMaxOwnerTaskDiagnosticBytes + 1, 'd');
    assert(!session->DeliverTaskResult(failed));
    failed.diagnostic.assign("A\0B", 3);
    assert(!session->DeliverTaskResult(failed));
    failed.diagnostic.assign("\xC0\xAF", 2);
    assert(!session->DeliverTaskResult(failed));
    assert(fixture.Count() == 0 && !Submit(fixture, request.value));

    failed.diagnostic.assign(contracts::kMaxOwnerTaskDiagnosticBytes, 'd');
    const auto original = failed;
    sinks.mutate_result = &failed;
    assert(session->DeliverTaskResult(failed));
    assert(failed.request_id == 0 && failed.diagnostic.empty());
    const auto &record = fixture.Record(0);
    assert(record.request_id == original.request_id && record.outcome == PRISM_TASK_FAILED_V1 &&
           record.failure_code == PRISM_TASK_OPERATION_FAILED_V1 && !record.choice_id &&
           !record.cancel_reason);
    assert(std::string_view(record.diagnostic, record.diagnostic_size) == original.diagnostic);
    sinks.mutate_result = nullptr;
    assert(!session->DeliverTaskResult(original));
    const auto next = Submit(fixture, request.value);
    assert(next > id && session->DeliverTaskResult(Cancelled(next)));
    assert(fixture.Count() == 2 && fixture.Errors() == 0);
}

void CheckFileLexicalContracts()
{
    using namespace contracts;
    assert(OwnerTaskCapability(OwnerTaskKind::Confirmation) == 1);
    assert(OwnerTaskCapability(OwnerTaskKind::OpenFile) == 2);
    assert(OwnerTaskCapability(OwnerTaskKind::SaveFile) == 4);
    assert(OwnerTaskCapability(OwnerTaskKind::SelectDirectory) == 8);
    assert(!OwnerTaskCapability(static_cast<OwnerTaskKind>(99)));

    assert(ValidOwnerFilePath("/") && ValidOwnerFilePath("/home/ss/笔记.md"));
    assert(ValidOwnerFilePath("/home/ss/a\\b.md"));
    assert(ValidOwnerFileDirectoryHint("") &&
           ValidOwnerFileDirectoryHint("/home/ss/../ss/./Documents//"));
    for (const auto &invalid :
         {std::string{}, std::string("relative/file"), std::string("/home//ss"),
          std::string("/home/./ss"), std::string("/home/../ss"), std::string("/home/ss/"),
          std::string("/home/A\0B", 9), std::string("/home/A\tB"), std::string("/home/\xC0\xAF", 8),
          std::string("/home/\xC2\x85", 8), std::string("/home/\xE2\x80\xA8", 9),
          std::string("/") + std::string(256, 'n')}) {
        assert(!ValidOwnerFilePath(invalid));
    }
    assert(!ValidOwnerFileDirectoryHint("relative") && !ValidOwnerFileDirectoryHint("/home/A\nB"));
    std::string maximum_path;
    for (unsigned at = 0; at < 16; ++at) {
        maximum_path += '/' + std::string(255, 'n');
    }
    assert(maximum_path.size() == kMaxOwnerFileTaskPathBytes && ValidOwnerFilePath(maximum_path));
    assert(!ValidOwnerFilePath(maximum_path + "/n"));

    assert(ValidOwnerFileName("笔记.md") && ValidOwnerFileName("a\\b.md") &&
           ValidOwnerFileName(std::string(255, 'n')));
    for (const auto &invalid : {std::string{}, std::string("."), std::string(".."),
                                std::string("a/b"), std::string("A\0B", 3), std::string("A\tB"),
                                std::string("\xED\xA0\x80", 3), std::string(256, 'n')}) {
        assert(!ValidOwnerFileName(invalid));
    }

    const std::vector<std::string> extensions{".md", ".tar.gz", ".a_B-2"};
    assert(ValidOwnerFileExtensions(extensions) && ValidOwnerFileExtensions({}));
    assert(OwnerFileExtensionMatches("archive.tar.gz", extensions));
    assert(OwnerFileExtensionMatches("notes.md", extensions));
    assert(!OwnerFileExtensionMatches("notes.MD", extensions));
    assert(!OwnerFileExtensionMatches("archive.gz", extensions));
    assert(OwnerFileExtensionMatches("any.extension", {}));
    assert(!OwnerFileExtensionMatches("a/b.md", extensions));
    for (const auto &invalid :
         std::vector<std::vector<std::string>>{{""},
                                               {"md"},
                                               {"."},
                                               {"..md"},
                                               {".tar..gz"},
                                               {".md."},
                                               {".m/d"},
                                               {".m\\d"},
                                               {".é"},
                                               {".m d"},
                                               {".md", ".md"},
                                               {"." + std::string(32, 'a')}}) {
        assert(!ValidOwnerFileExtensions(invalid));
    }
    std::vector<std::string> maximum_extensions;
    for (unsigned at = 0; at < kMaxOwnerFileTaskExtensions; ++at) {
        maximum_extensions.push_back(".extension_" + std::to_string(at));
    }
    assert(ValidOwnerFileExtensions(maximum_extensions));
    maximum_extensions.push_back(".extra");
    assert(!ValidOwnerFileExtensions(maximum_extensions));

    OwnerTaskRequest request;
    request.request_id = 1;
    request.kind = OwnerTaskKind::SaveFile;
    request.title = "Save a document";
    request.file = OwnerFileTaskOptions{"/home/ss/../ss", "notes.md", {".md"}, true};
    assert(ValidateOwnerTaskRequest(request));
    auto mixed = request;
    mixed.choices.push_back({7, "Save", OwnerTaskChoiceRole::Primary});
    assert(!ValidateOwnerTaskRequest(mixed));
    mixed = request;
    mixed.kind = OwnerTaskKind::Confirmation;
    assert(!ValidateOwnerTaskRequest(mixed));
    mixed = request;
    mixed.kind = OwnerTaskKind::OpenFile;
    assert(!ValidateOwnerTaskRequest(mixed));
    mixed.file->suggested_name.clear();
    assert(ValidateOwnerTaskRequest(mixed));
    mixed.kind = OwnerTaskKind::SelectDirectory;
    assert(!ValidateOwnerTaskRequest(mixed));
    mixed.file->extensions.clear();
    assert(ValidateOwnerTaskRequest(mixed));
    auto result = FileSuccess(1, "/");
    assert(ValidateOwnerTaskResult(result, mixed));
    result.overwrite_approved = true;
    assert(!ValidateOwnerTaskResult(result, mixed));

    assert(ValidateOwnerTaskResult(FileSuccess(1, "/home/ss/notes.md", true), request));
    assert(!ValidateOwnerTaskResult(FileSuccess(1, "/home/ss/notes.MD"), request));
    assert(!ValidateOwnerTaskResult(FileSuccess(1, "/"), request));
    assert(!ValidateOwnerTaskResult(FileSuccess(1, "/home/ss/../notes.md"), request));
}

void CheckFileCopiedInputAndCapabilities(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    sinks.capabilities = contracts::kOwnerTaskCapabilities | 0x80000000u;
    auto session = Open(path, sinks);
    const auto *host = fixture.Host();
    assert(host->task_capabilities(host->context) == contracts::kOwnerTaskCapabilities);

    BorrowedFileRequest request;
    const auto id = Submit(fixture, request.value);
    assert(id && sinks.requests.size() == 1 && fixture.Count() == 0);
    const auto expected = sinks.requests.back();
    assert(expected.kind == contracts::OwnerTaskKind::OpenFile && expected.choices.empty() &&
           expected.file && expected.file->initial_directory == request.directory &&
           expected.file->suggested_name.empty() &&
           expected.file->extensions == request.extensions && expected.file->show_hidden);

    request.title.assign("Changed title");
    request.message.assign("Changed message");
    request.directory.assign("/changed");
    request.extensions.assign({".changed"});
    request.options.flags = 0;
    request.extension_views.clear();
    assert(sinks.requests.back() == expected);

    auto result = FileSuccess(id, "/home/ss/Documents/笔记.md");
    const auto original = result;
    sinks.mutate_result = &result;
    assert(session->DeliverTaskResult(result));
    assert(!result.request_id && result.file_path.empty());
    const auto &record = fixture.Record(0);
    assert(record.request_id == id && !record.choice_id && !record.overwrite_approved &&
           std::string_view(record.file_path, record.file_path_size) == original.file_path);
    sinks.mutate_result = nullptr;
    assert(!session->DeliverTaskResult(original));

    request.Refresh();
    sinks.capabilities = contracts::kOwnerTaskSaveFileCapability;
    const auto count = sinks.requests.size();
    assert(!Submit(fixture, request.value));
    BorrowedRequest confirmation;
    assert(!Submit(fixture, confirmation.value));
    assert(sinks.requests.size() == count);
    request.kind = PRISM_TASK_SAVE_FILE_V1;
    request.name = "Report.changed";
    request.Refresh();
    const auto save = Submit(fixture, request.value);
    assert(save > id);
    assert(session->DeliverTaskResult(FileSuccess(save, "/changed/Report.changed", true)));
    assert(fixture.Record(1).overwrite_approved == 1);

    request.kind = PRISM_TASK_SELECT_DIRECTORY_V1;
    request.name.clear();
    request.extensions.clear();
    request.Refresh();
    assert(!Submit(fixture, request.value));
    sinks.capabilities = contracts::kOwnerTaskSelectDirectoryCapability;
    const auto directory = Submit(fixture, request.value);
    assert(directory > save && session->DeliverTaskResult(FileSuccess(directory, "/")));
    assert(fixture.Count() == 3 && fixture.Errors() == 0);
}

void CheckFileRequestValidation(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    sinks.capabilities = contracts::kOwnerTaskCapabilities;
    auto session = Open(path, sinks);
    BorrowedFileRequest request;
    request.value.file = nullptr;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.value.struct_size = sizeof(request.value) - 1;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.options.struct_size =
        offsetof(PrismFileTaskOptionsV1, flags) + sizeof(request.options.flags) - 1;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.options.flags |= 2;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.options.initial_directory = {nullptr, 1};
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.options.suggested_name = {nullptr, 1};
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.options.extensions = nullptr;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.options.extensions_size = contracts::kMaxOwnerFileTaskExtensions + 1;
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.extension_views[0] = {nullptr, 1};
    RejectRequest(fixture, &request.value, sinks);
    request.Refresh();
    request.name = "Open cannot suggest a name.md";
    request.Refresh();
    RejectRequest(fixture, &request.value, sinks);
    request.kind = PRISM_TASK_SELECT_DIRECTORY_V1;
    request.name.clear();
    request.Refresh();
    RejectRequest(fixture, &request.value, sinks);

    request.kind = PRISM_TASK_SAVE_FILE_V1;
    request.extensions = {".md"};
    for (const auto &invalid :
         {std::string("."), std::string(".."), std::string("a/b.md"), std::string("A\0B", 3),
          std::string(contracts::kMaxOwnerFileTaskNameBytes + 1, 'n')}) {
        request.name = invalid;
        request.Refresh();
        RejectRequest(fixture, &request.value, sinks);
    }
    request.name = "notes.md";
    for (const auto &invalid :
         {std::string("relative"), std::string("/A\0B", 4), std::string("/A\nB"),
          std::string("/") + std::string(contracts::kMaxOwnerFileTaskNameBytes + 1, 'n')}) {
        request.directory = invalid;
        request.Refresh();
        RejectRequest(fixture, &request.value, sinks);
    }
    request.directory.clear();
    request.extensions = {".md", ".md"};
    request.Refresh();
    RejectRequest(fixture, &request.value, sinks);

    BorrowedRequest confirmation;
    confirmation.value.file = &request.options;
    RejectRequest(fixture, &confirmation.value, sinks);

    request.extensions.clear();
    request.Refresh();
    request.options.initial_directory = {nullptr, 0};
    request.options.extensions = nullptr;
    request.value.struct_size += 8;
    request.options.struct_size += 8;
    const auto id = Submit(fixture, request.value);
    assert(id && sinks.requests.back().file->initial_directory.empty() &&
           sinks.requests.back().file->extensions.empty());
    assert(session->DeliverTaskResult(FileSuccess(id, "/home/ss/notes.md")));
    assert(fixture.Count() == 1 && fixture.Errors() == 0);
}

void CheckFileResultsAndReentry(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    sinks.capabilities = contracts::kOwnerTaskCapabilities;
    auto session = Open(path, sinks);
    BorrowedFileRequest request;
    const auto id = Submit(fixture, request.value);
    assert(id);
    for (const auto &invalid :
         {FileSuccess(id, ""), FileSuccess(id, "notes.md"), FileSuccess(id, "/"),
          FileSuccess(id, "/home/ss/../notes.md"), FileSuccess(id, "/home//ss/notes.md"),
          FileSuccess(id, "/home/ss/notes.md/"), FileSuccess(id, "/home/ss/notes.txt"),
          FileSuccess(id, "/home/ss/notes.md", true), FileSuccess(id + 1, "/home/ss/notes.md")}) {
        assert(!session->DeliverTaskResult(invalid));
    }
    auto invalid = FileSuccess(id, "/home/ss/notes.md");
    invalid.choice_id = 11;
    assert(!session->DeliverTaskResult(invalid));
    invalid = FileSuccess(id, "/home/ss/notes.md");
    invalid.diagnostic = "Mixed success payload";
    assert(!session->DeliverTaskResult(invalid));
    invalid = Cancelled(id);
    invalid.file_path = "/home/ss/notes.md";
    assert(!session->DeliverTaskResult(invalid));
    invalid.file_path.clear();
    invalid.overwrite_approved = true;
    assert(!session->DeliverTaskResult(invalid));
    invalid = {};
    invalid.request_id = id;
    invalid.outcome = contracts::OwnerTaskOutcome::Failed;
    invalid.failure_code = contracts::OwnerTaskFailureCode::OperationFailed;
    invalid.file_path = "/home/ss/notes.md";
    assert(!session->DeliverTaskResult(invalid));
    assert(fixture.Count() == 0);

    const auto *host = fixture.Host();
    assert(host->cancel_task(host->context, id) == 0);
    assert(!session->DeliverTaskResult(FileSuccess(id, "/home/ss/notes.md")));
    assert(!Submit(fixture, request.value));
    assert(session->DeliverTaskResult(Cancelled(id)));

    const auto second = Submit(fixture, request.value);
    fixture.Reenter();
    assert(session->DeliverTaskResult(FileSuccess(second, "/home/ss/archive.tar.gz")));
    const auto next = fixture.NextRequest();
    assert(next > second && sinks.requests.back().kind == contracts::OwnerTaskKind::Confirmation);
    assert(!session->DeliverTaskResult(FileSuccess(second, "/home/ss/archive.tar.gz")));
    assert(session->DeliverTaskResult(Success(next, 7)));
    assert(fixture.Count() == 3 && fixture.Errors() == 0);
}

void CheckCancellationAndCallbackReentry(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    auto session = Open(path, sinks);
    BorrowedRequest request;
    const auto *host = fixture.Host();
    const auto id = Submit(fixture, request.value);
    assert(host->cancel_task(host->context, 0) == -1);
    assert(host->cancel_task(host->context, id + 1) == -1);
    assert(sinks.cancellations.empty());
    sinks.cancel_accept = false;
    assert(host->cancel_task(host->context, id) == -1);
    assert(sinks.cancellations.size() == 1);
    sinks.cancel_accept = true;
    sinks.attempt_inline_result = true;
    assert(host->cancel_task(host->context, id) == 0);
    assert(!sinks.inline_result_accepted && fixture.Count() == 0);
    assert(host->cancel_task(host->context, id) == 0);
    assert(sinks.cancellations.size() == 2);
    assert(!session->DeliverTaskResult(Success(id)));
    assert(!Submit(fixture, request.value));
    assert(session->DeliverTaskResult(Cancelled(id)));
    assert(fixture.Count() == 1 && fixture.Record(0).outcome == PRISM_TASK_CANCELLED_V1);

    sinks.attempt_inline_result = false;
    const auto prior = Submit(fixture, request.value);
    fixture.Reenter();
    assert(session->DeliverTaskResult(Success(prior)));
    const auto next = fixture.NextRequest();
    assert(next > prior && sinks.requests.back().request_id == next && fixture.Count() == 2);
    assert(!session->DeliverTaskResult(Success(prior)));
    assert(!Submit(fixture, request.value));
    assert(!session->DeliverTaskResult(Success(next, 11)));
    assert(session->DeliverTaskResult(Success(next, 7)));
    assert(fixture.Count() == 3 && sinks.completions == 3 && fixture.Errors() == 0);
}

void CheckSubmissionGuardsAndProviderErrors(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    auto session = Open(path, sinks);
    BorrowedRequest request;
    sinks.host = fixture.Host();
    sinks.nested_request = &request.value;
    sinks.attempt_inline_result = true;
    sinks.attempt_nested_submit = true;
    sinks.attempt_capability_submit = true;
    const auto id = Submit(fixture, request.value);
    assert(id && sinks.requests.size() == 1 && !sinks.inline_result_accepted && !sinks.nested_id);
    assert(fixture.Count() == 0);
    assert(session->DeliverTaskResult(Success(id)));
    sinks.attempt_inline_result = false;
    sinks.attempt_nested_submit = false;
    sinks.attempt_capability_submit = false;

    sinks.accept = false;
    assert(!Submit(fixture, request.value));
    const auto rejected = sinks.requests.back().request_id;
    assert(rejected > id && !session->DeliverTaskResult(Success(rejected)));
    sinks.accept = true;
    sinks.throw_submit = true;
    assert(!Submit(fixture, request.value));
    const auto thrown = sinks.requests.back().request_id;
    assert(thrown > rejected && sinks.cancellations.back() == thrown);
    assert(!session->DeliverTaskResult(Cancelled(thrown)));
    sinks.throw_submit = false;
    const auto last = Submit(fixture, request.value);
    assert(last > thrown);
    sinks.throw_cancel = true;
    assert(fixture.Host()->cancel_task(fixture.Host()->context, last) == -1);
    sinks.throw_cancel = false;
    assert(session->DeliverTaskResult(Success(last)));
    assert(fixture.Count() == 2 && fixture.Errors() == 0);

    sinks.capabilities = 0;
    const auto calls = sinks.requests.size();
    assert(!Submit(fixture, request.value));
    assert(!fixture.Host()->task_capabilities(fixture.Host()->context));
    sinks.capabilities = 0x80000001u;
    assert(fixture.Host()->task_capabilities(fixture.Host()->context) == 1);
    sinks.throw_capabilities = true;
    assert(!Submit(fixture, request.value));
    assert(!fixture.Host()->task_capabilities(fixture.Host()->context));
    assert(sinks.requests.size() == calls);
}

void CheckThreadAndPermanentClose(const char *path)
{
    Fixture fixture(path);
    Sinks sinks;
    auto session = Open(path, sinks);
    BorrowedRequest request;
    const auto id = Submit(fixture, request.value);
    const auto caps = sinks.capability_calls;
    WrongThreadCall wrong{fixture.Host(), &request.value, session.get(), Success(id)};
    std::thread worker(std::ref(wrong));
    worker.join();
    assert(!wrong.capabilities && !wrong.requested && wrong.cancelled == -1 && !wrong.delivered);
    assert(sinks.requests.size() == 1 && sinks.cancellations.empty() &&
           sinks.capability_calls == caps && fixture.Count() == 0);
    session->StopWork();
    session->StopWork();
    assert(!session->SupportsOwnerTasks());
    assert(!fixture.Host()->task_capabilities(fixture.Host()->context));
    assert(!Submit(fixture, request.value));
    assert(fixture.Host()->cancel_task(fixture.Host()->context, id) == -1);
    assert(!session->DeliverTaskResult(Success(id)));
    assert(!session->DeliverTaskResult(Cancelled(id)));
    assert(fixture.Count() == 0 && sinks.cancellations.empty());
}

void CheckOptionalProviderCapabilities(const char *path)
{
    for (unsigned omitted = 1; omitted <= 3; ++omitted) {
        Fixture fixture(path);
        Sinks sinks;
        auto session = Open(path, sinks, omitted);
        BorrowedRequest request;
        assert(session->SupportsOwnerTasks()); // The module callback still exists.
        assert(!fixture.Host()->task_capabilities(fixture.Host()->context));
        assert(!Submit(fixture, request.value));
        assert(sinks.requests.empty() && sinks.cancellations.empty() &&
               sinks.capability_calls == 0 && fixture.Count() == 0);
    }
}

void CheckSynchronousStopWork(const char *path)
{
    for (unsigned boundary = 0; boundary < 3; ++boundary) {
        Fixture fixture(path);
        Sinks sinks;
        auto session = Open(path, sinks);
        BorrowedRequest request;
        std::uint64_t id{};
        if (boundary == 0) {
            sinks.stop_submit = true;
            assert(!Submit(fixture, request.value));
            assert(sinks.requests.size() == 1);
            id = sinks.requests.back().request_id;
        } else if (boundary == 1) {
            id = Submit(fixture, request.value);
            assert(id);
            sinks.stop_cancel = true;
            assert(fixture.Host()->cancel_task(fixture.Host()->context, id) == -1);
            assert(sinks.cancellations.size() == 1);
        } else {
            sinks.stop_capabilities = true;
            assert(!fixture.Host()->task_capabilities(fixture.Host()->context));
            assert(sinks.requests.empty());
        }
        assert(!session->SupportsOwnerTasks());
        assert(!Submit(fixture, request.value));
        assert(!session->DeliverTaskResult(Success(id)));
        assert(!session->DeliverTaskResult(Cancelled(id)));
        assert(fixture.Count() == 0);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 4);
    CheckAbiPrefixes(argv[1], argv[2], argv[3]);
    CheckGuardedRequestPrefixes(argv[1]);
    CheckCopiedInputAndSinglePending(argv[1]);
    CheckRequestValidation(argv[1]);
    CheckResultsAndOwnedProjection(argv[1]);
    CheckFileLexicalContracts();
    CheckFileCopiedInputAndCapabilities(argv[1]);
    CheckFileRequestValidation(argv[1]);
    CheckFileResultsAndReentry(argv[1]);
    CheckCancellationAndCallbackReentry(argv[1]);
    CheckSubmissionGuardsAndProviderErrors(argv[1]);
    CheckThreadAndPermanentClose(argv[1]);
    CheckOptionalProviderCapabilities(argv[1]);
    CheckSynchronousStopWork(argv[1]);
}
