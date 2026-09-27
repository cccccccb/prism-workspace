#include "module_work_fixture.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <new>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace module_work_fixture;

struct Instance {
    const PrismHostApiV1 *host;
    std::thread::id owner;
    bool ready{};
};

const PrismHostApiV1 *host;
std::thread::id owner;
std::array<Record, 256> records;
std::size_t record_count;
std::atomic<unsigned> active{}, errors{};
int lifecycle_fd{-1}, create_entered{-1}, create_gate{-1}, completed_entered{-1},
    completed_gate{-1};

void Signal(int fd)
{
    if (fd >= 0) {
        const std::uint64_t value = 1;
        if (write(fd, &value, sizeof(value)) != sizeof(value)) {
            errors.fetch_or(1);
        }
    }
}

void Wait(int fd)
{
    if (fd >= 0) {
        pollfd source{fd, POLLIN, 0};
        if (poll(&source, 1, 5000) != 1 || !(source.revents & POLLIN)) {
            errors.fetch_or(2);
        }
    }
}

void Log(char event, int fd)
{
    if (fd >= 0 && write(fd, &event, 1) != 1) {
        errors.fetch_or(4);
    }
}

struct WorkLifetime {
    int exited;

    explicit WorkLifetime(int fd) : exited(fd)
    {
        ++active;
        if (std::this_thread::get_id() == owner) {
            errors.fetch_or(8);
        }
    }

    ~WorkLifetime()
    {
        --active;
        Log('E', exited);
    }
};

int32_t Empty(const PrismWorkContextV1 *, PrismBytesViewV1)
{
    return 0;
}

std::uint16_t TryForbiddenHostCalls()
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_NUMBER_V1;
    value.as.number = 7;
    PrismWorkRequestV1 request{};
    request.struct_size = sizeof(request);
    request.task_id = 9999;
    request.reserve_bytes = 4096;
    request.work = Empty;
    std::uint16_t rejected = 0;
    rejected |= (host->set_binding(host->context, {"forbidden", 9}, value) != 0) << 0;
    rejected |= (host->backend_ready(host->context) != 0) << 1;
    rejected |= (host->launch_app(host->context, {"music", 5}) == 0) << 2;
    rejected |= (host->schedule_tick(host->context, 1) != 0) << 3;
    rejected |= (host->subscribe_instances(host->context) == 0) << 4;
    rejected |= (host->select_theme(host->context, {"glass", 5}) == 0) << 5;
    rejected |= (host->select_color_scheme(host->context, {"light", 5}) == 0) << 6;
    rejected |= (host->submit_work(host->context, &request) == PRISM_WORK_WRONG_THREAD_V1) << 7;
    rejected |= (host->cancel_work(host->context, 1) == PRISM_WORK_WRONG_THREAD_V1) << 8;
    return rejected;
}

int32_t Work(const PrismWorkContextV1 *context, PrismBytesViewV1 bytes)
{
    if (!context || context->struct_size < sizeof(*context) || bytes.size < sizeof(Input)) {
        return 91;
    }
    Input input;
    std::memcpy(&input, bytes.data, sizeof(input));
    WorkLifetime lifetime(input.exited);
    Signal(input.entered);
    if (input.gate >= 0) {
        pollfd sources[2]{{input.gate, POLLIN, 0}, {context->cancellation_fd, POLLIN, 0}};
        if (poll(sources, 2, 5000) < 1) {
            return 92;
        }
    }
    const PrismBytesViewV1 payload{bytes.data + sizeof(Input), bytes.size - sizeof(Input)};
    if (input.mode == Mode::Empty) {
        return 0;
    }
    if (input.mode == Mode::Forbidden) {
        const auto rejected = TryForbiddenHostCalls();
        return context->set_result(
            context->context, {reinterpret_cast<const uint8_t *>(&rejected), sizeof(rejected)});
    }
    if (input.mode == Mode::Late && !context->is_cancelled(context->context)) {
        return 93;
    }
    const auto written = context->set_result(context->context, payload);
    if (input.mode == Mode::Overflow) {
        return written < 0 ? 0 : 94;
    }
    if (input.mode == Mode::RepeatedResult) {
        return written == 0 && context->set_result(context->context, payload) < 0 ? 0 : 95;
    }
    return input.mode == Mode::Failure ? 17 : 0;
}

void *Create(const PrismAppInitV1 *init)
{
    if (!init || !init->host || init->host->struct_size < sizeof(PrismHostApiV1)) {
        return nullptr;
    }
    auto *instance = new (std::nothrow) Instance{init->host, std::this_thread::get_id()};
    if (!instance) {
        return nullptr;
    }
    host = init->host;
    owner = instance->owner;
    if (create_gate >= 0) {
        instance->ready = host->backend_ready(host->context) == 0;
        Signal(create_entered);
        Wait(create_gate);
    }
    return instance;
}

void Destroy(void *pointer)
{
    auto *instance = static_cast<Instance *>(pointer);
    if (active.load() != 0 || std::this_thread::get_id() != instance->owner) {
        errors.fetch_or(16);
    }
    Log('D', lifecycle_fd);
    delete instance;
}

void Tick(void *, uint64_t)
{
}

void Completed(void *pointer, const PrismWorkCompletionV1 *completion)
{
    auto &instance = *static_cast<Instance *>(pointer);
    if (std::this_thread::get_id() != instance.owner || record_count == records.size() ||
        completion->result.size > sizeof(Record::bytes)) {
        errors.fetch_or(32);
        return;
    }
    if (completed_gate >= 0) {
        Signal(completed_entered);
        Wait(completed_gate);
        completed_gate = completed_entered = -1;
    }
    auto &record = records[record_count++];
    record = {completion->task_id, completion->status, completion->error_code,
              completion->result.size};
    if (record.size) {
        std::memcpy(record.bytes, completion->result.data, record.size);
    }
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_NUMBER_V1;
    value.as.number = static_cast<double>(completion->task_id);
    if (instance.host->set_binding(instance.host->context, {"completed", 9}, value) != 0) {
        errors.fetch_or(64);
    }
    if (completion->status == PRISM_WORK_SUCCEEDED_V1 && !instance.ready) {
        instance.ready = instance.host->backend_ready(instance.host->context) == 0;
        if (!instance.ready) {
            errors.fetch_or(128);
        }
    }
}

__attribute__((destructor)) void Unloaded()
{
    Log('U', lifecycle_fd);
}
} // namespace

extern "C" {
PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    static const PrismAppModuleV1 api{sizeof(api), PRISM_APP_ABI_V1, Create,  Destroy, nullptr,
                                      Tick,        nullptr,          nullptr, nullptr, Completed};
    return &api;
}

PRISM_APP_EXPORT const PrismHostApiV1 *module_work_host()
{
    return host;
}

PRISM_APP_EXPORT PrismWorkFunctionV1 module_work_function()
{
    return Work;
}

PRISM_APP_EXPORT const Record *module_work_record(std::size_t index)
{
    return index < record_count ? &records[index] : nullptr;
}

PRISM_APP_EXPORT std::size_t module_work_count()
{
    return record_count;
}

PRISM_APP_EXPORT unsigned module_work_errors()
{
    return errors.load();
}

PRISM_APP_EXPORT void module_work_reset()
{
    record_count = 0;
    errors = 0;
    lifecycle_fd = create_entered = create_gate = completed_entered = completed_gate = -1;
}

PRISM_APP_EXPORT void module_work_lifecycle(int fd)
{
    lifecycle_fd = fd;
}

PRISM_APP_EXPORT void module_work_create_gate(int entered, int gate)
{
    create_entered = entered;
    create_gate = gate;
}

PRISM_APP_EXPORT void module_work_completion_gate(int entered, int gate)
{
    completed_entered = entered;
    completed_gate = gate;
}
}
