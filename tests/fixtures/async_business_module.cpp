#include "prism/contracts/app_module.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
enum EventKind : std::uint32_t {
    Created = 1,
    Entered,
    Finished,
    Completed,
    ThemeReceived,
    LaunchReceived,
    InstanceReceived,
    Cancelled,
    Destroyed,
    Unloaded
};

struct Event {
    std::uint32_t kind{}, task{}, status{}, active{};
    std::int32_t error{};
    std::uint64_t thread{};
};

struct Input {
    int gate{}, events{}, task{}, fail{};
    std::uint64_t owner{};
};

struct Result {
    int task{}, value{};
};

std::atomic<unsigned> active{};
int report_fd = -1;
bool destroyed = false;

void Require(bool condition, const char *detail)
{
    if (!condition) {
        std::fprintf(stderr, "async_business_fixture: %s\n", detail);
        std::abort();
    }
}

std::uint64_t Thread()
{
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
}

void Emit(int fd, Event event)
{
    event.thread = Thread();
    ssize_t sent;
    do {
        sent = send(fd, &event, sizeof(event), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    Require(sent == sizeof(event), "Fixture event send failed");
}

int EnvironmentFd(const char *name)
{
    const auto *text = std::getenv(name);
    Require(text != nullptr, "Missing fixture descriptor");
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    Require(end && !*end && value >= 0 && value <= 1000000, "Invalid fixture descriptor");
    return static_cast<int>(value);
}

struct State {
    const PrismHostApiV1 *host{};
    std::uint64_t owner{};
    unsigned completions{};
    bool failure{};
};

void SetText(State &state, std::string_view text)
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_STRING_V1;
    value.as.string = {text.data(), text.size()};
    Require(state.host->set_binding(state.host->context, {"status", 6}, value) == 0,
            "Host rejected status binding");
}

void SetProgress(State &state, double progress)
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_NUMBER_V1;
    value.as.number = progress;
    Require(state.host->set_binding(state.host->context, {"progress", 8}, value) == 0,
            "Host rejected progress binding");
}

int32_t Work(const PrismWorkContextV1 *context, PrismBytesViewV1 bytes)
{
    Require(context && bytes.size == sizeof(Input), "Invalid copied work input");
    Input input;
    std::memcpy(&input, bytes.data, sizeof(input));
    Require(Thread() != input.owner, "Work ran on the UI owner thread");
    const auto simultaneous = active.fetch_add(1) + 1;
    Emit(input.events, {Entered, static_cast<unsigned>(input.task), 0, simultaneous});
    pollfd fds[] = {{input.gate, POLLIN, 0}, {context->cancellation_fd, POLLIN, 0}};
    int count;
    do {
        count = poll(fds, 2, -1);
    } while (count < 0 && errno == EINTR);
    Require(count > 0, "Work barrier poll failed");
    if (context->is_cancelled(context->context) || fds[1].revents) {
        active.fetch_sub(1);
        Emit(input.events, {Cancelled, static_cast<unsigned>(input.task)});
        return 77;
    }
    Require(fds[0].revents & POLLIN, "Work barrier did not release");
    std::uint64_t release;
    Require(read(input.gate, &release, sizeof(release)) == sizeof(release), "Invalid gate release");
    if (input.fail) {
        active.fetch_sub(1);
        Emit(input.events,
             {Finished, static_cast<unsigned>(input.task), PRISM_WORK_FAILED_V1, 0, 73});
        return 73;
    }
    const Result result{input.task, input.task * 100 + 7};
    Require(context->set_result(context->context, {reinterpret_cast<const std::uint8_t *>(&result),
                                                   sizeof(result)}) == 0,
            "Host rejected bounded copied work result");
    active.fetch_sub(1);
    Emit(input.events, {Finished, static_cast<unsigned>(input.task), PRISM_WORK_SUCCEEDED_V1});
    return 0;
}

void *Create(const PrismAppInitV1 *init)
{
    Require(init && init->host && init->host->submit_work && init->host->cancel_work,
            "Host lacks async work ABI tail");
    report_fd = EnvironmentFd("PRISM_TEST_BUSINESS_EVENTS");
    destroyed = false;
    auto *state = new State{init->host, Thread()};
    const std::string_view id(init->app_id.data, init->app_id.size);
    SetText(*state, "Business work pending");
    SetProgress(*state, 0);
    Require(state->host->launch_app(state->host->context, {"business-other", 14}) == 9,
            "Host failed deterministic launch registration");
    Require(state->host->subscribe_instances(state->host->context) == 9,
            "Host failed deterministic subscription registration");
    for (int task = 1; task <= 2; ++task) {
        const Input input{
            EnvironmentFd(task == 1 ? "PRISM_TEST_BUSINESS_GATE1" : "PRISM_TEST_BUSINESS_GATE2"),
            report_fd, task, task == 2 && id.ends_with("failure"), state->owner};
        PrismWorkRequestV1 request{};
        request.struct_size = sizeof(request);
        request.task_id = task;
        request.priority = PRISM_WORK_CRITICAL_V1;
        request.reserve_bytes = 128 * 1024;
        request.max_result_bytes = sizeof(Result);
        request.input = {reinterpret_cast<const std::uint8_t *>(&input), sizeof(input)};
        request.work = Work;
        Require(state->host->submit_work(state->host->context, &request) == PRISM_WORK_ACCEPTED_V1,
                "Host rejected fixture work");
    }
    Emit(report_fd, {Created});
    return state;
}

void Complete(void *value, const PrismWorkCompletionV1 *completion)
{
    auto &state = *static_cast<State *>(value);
    Require(!destroyed && Thread() == state.owner && completion,
            "Invalid owner completion lifetime");
    Require(completion->task_id == 1 || completion->task_id == 2, "Unexpected task completion ID");
    if (completion->status == PRISM_WORK_SUCCEEDED_V1) {
        Require(completion->result.size == sizeof(Result), "Unexpected success result size");
        Result result;
        std::memcpy(&result, completion->result.data, sizeof(result));
        Require(result.task == static_cast<int>(completion->task_id) &&
                    result.value == result.task * 100 + 7,
                "Copied task result changed");
    } else {
        Require(completion->status == PRISM_WORK_FAILED_V1 && completion->error_code == 73 &&
                    completion->result.size == 0,
                "Failure lost typed status/error");
        state.failure = true;
    }
    ++state.completions;
    SetProgress(state, state.completions * .5);
    SetText(state, state.failure ? "Typed work failure handled" : "Business result received");
    Emit(report_fd, {Completed, static_cast<unsigned>(completion->task_id), completion->status, 0,
                     completion->error_code});
    if (state.completions == 2) {
        Require(state.host->backend_ready(state.host->context) == 0, "Host rejected owner Ready");
    }
}

void Theme(void *value, const PrismThemeEventV1 *event)
{
    Require(event && event->request_id == 0, "Expected registered broadcast theme event");
    auto &state = *static_cast<State *>(value);
    SetText(state, "Business theme control received");
    Emit(report_fd, {ThemeReceived});
}

void Launch(void *value, const PrismLaunchEventV1 *event)
{
    Require(event && event->request_id == 9, "Expected registered launch event");
    SetText(*static_cast<State *>(value), "Business launch control received");
    Emit(report_fd, {LaunchReceived});
}

void Instance(void *value, const PrismInstanceEventV1 *event)
{
    Require(event && event->subscription_id == 9, "Expected registered instance event");
    SetText(*static_cast<State *>(value), "Business instance control received");
    Emit(report_fd, {InstanceReceived});
}

void Destroy(void *value)
{
    auto *state = static_cast<State *>(value);
    Require(Thread() == state->owner && active.load() == 0, "Destroy preceded work join");
    PrismValueV1 data{};
    data.kind = PRISM_VALUE_STRING_V1;
    data.as.string = {"after-close", 11};
    Require(state->host->set_binding(state->host->context, {"status", 6}, data) != 0,
            "Host API stayed open during destroy");
    destroyed = true;
    Emit(report_fd, {Destroyed});
    delete state;
}

struct Lifetime {
    ~Lifetime()
    {
        if (report_fd >= 0) {
            Emit(report_fd, {Unloaded});
        }
    }
} lifetime;

const PrismAppModuleV1 module{sizeof(module), PRISM_APP_ABI_V1, Create,   Destroy, nullptr,
                              nullptr,        Launch,           Instance, Theme,   Complete};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &module;
}
