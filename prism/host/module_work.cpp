#include "module_work_p.hpp"
#include <algorithm>
#include <limits>

namespace prism::sdk {
namespace {
constexpr std::uint64_t WorkOverhead = 64 * 1024;

bool ValidRequest(const PrismWorkRequestV1 &request, const ModuleSessionLimits &limits)
{
    if (request.struct_size < sizeof(request) || !request.task_id || !request.work ||
        (!request.input.data && request.input.size) || request.input.size > limits.input_bytes ||
        request.max_result_bytes > limits.result_bytes ||
        (request.priority != PRISM_WORK_CRITICAL_V1 &&
         request.priority != PRISM_WORK_DEFERRED_V1)) {
        return false;
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (request.max_result_bytes > maximum - WorkOverhead ||
        request.input.size > maximum - WorkOverhead - request.max_result_bytes) {
        return false;
    }
    return request.reserve_bytes >= request.input.size + request.max_result_bytes + WorkOverhead;
}

int32_t ProjectSubmit(runtime::TaskSubmitResult result)
{
    switch (result) {
    case runtime::TaskSubmitResult::Accepted:
        return PRISM_WORK_ACCEPTED_V1;
    case runtime::TaskSubmitResult::Busy:
        return PRISM_WORK_BUSY_V1;
    case runtime::TaskSubmitResult::Closed:
        return PRISM_WORK_CLOSED_V1;
    case runtime::TaskSubmitResult::Invalid:
        return PRISM_WORK_INVALID_V1;
    }
    return PRISM_WORK_INVALID_V1;
}
} // namespace

int32_t ModuleSession::SubmitWork(void *context, const PrismWorkRequestV1 *request) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return PRISM_WORK_WRONG_THREAD_V1;
    }
    auto &self = *static_cast<ModuleSession *>(context);
    if (self.closed_) {
        return PRISM_WORK_CLOSED_V1;
    }
    if (!self.started_ || !self.module_.Api().on_work_completed || !request ||
        !ValidRequest(*request, self.limits_)) {
        return PRISM_WORK_INVALID_V1;
    }
    auto &work = *self.work_;
    if (work.pending.contains(request->task_id) ||
        work.pending.size() >= self.limits_.outstanding ||
        request->input.size > self.limits_.queued_input_bytes -
                                  std::min(work.input_bytes, self.limits_.queued_input_bytes)) {
        return PRISM_WORK_BUSY_V1;
    }

    try {
        std::vector<std::uint8_t> input;
        if (request->input.size) {
            input.assign(request->input.data, request->input.data + request->input.size);
        }
        runtime::TaskRequest task{
            request->task_id, static_cast<runtime::TaskPriority>(request->priority),
            request->reserve_bytes,
            ModuleWorkJob(request->work, std::move(input), request->max_result_bytes)};
        work.pending.emplace(request->task_id, ModuleWorkState::Pending{request->input.size});
        runtime::TaskSubmitResult result;
        try {
            result = work.channel->Submit(std::move(task));
        } catch (...) {
            work.pending.erase(request->task_id);
            throw;
        }
        if (result != runtime::TaskSubmitResult::Accepted) {
            work.pending.erase(request->task_id);
            return ProjectSubmit(result);
        }
        work.input_bytes += request->input.size;
        return PRISM_WORK_ACCEPTED_V1;
    } catch (...) {
        return PRISM_WORK_INVALID_V1;
    }
}

int32_t ModuleSession::CancelWork(void *context, std::uint64_t id) noexcept
{
    if (!context || !static_cast<ModuleSession *>(context)->OnOwnerThread()) {
        return PRISM_WORK_WRONG_THREAD_V1;
    }
    auto &self = *static_cast<ModuleSession *>(context);
    if (self.closed_) {
        return PRISM_WORK_CLOSED_V1;
    }
    auto &work = *self.work_;
    const auto found = work.pending.find(id);
    if (found == work.pending.end()) {
        return PRISM_WORK_INVALID_V1;
    }
    found->second.cancelled = true;
    try {
        work.channel->Cancel(id);
        return PRISM_WORK_ACCEPTED_V1;
    } catch (...) {
        return PRISM_WORK_INVALID_V1;
    }
}

int ModuleSession::WorkCompletionFd() const noexcept
{
    if (!OnOwnerThread() || closed_) {
        return -1;
    }
    return work_->channel->Fd();
}

bool ModuleSession::WorkPending() const noexcept
{
    return OnOwnerThread() && !closed_ && !work_->pending.empty();
}

void ModuleSession::StopWork() noexcept
{
    if (!OnOwnerThread() || closed_) {
        return;
    }
    closed_ = true;
    ready_ = false;
    tick_due_.reset();
    work_->channel->Stop();
    work_->pending.clear();
    work_->input_bytes = 0;
}

std::size_t ModuleSession::DispatchWork()
{
    if (!OnOwnerThread() || closed_ || !instance_) {
        return 0;
    }
    auto &work = *work_;
    const auto deadline = std::chrono::steady_clock::now() + limits_.dispatch_budget;
    std::size_t processed = 0;
    while (!closed_ && processed < limits_.completions_per_turn &&
           std::chrono::steady_clock::now() < deadline) {
        auto completion = work.channel->TakeCompletion();
        if (!completion) {
            break;
        }
        const auto found = work.pending.find(completion->id);
        if (found == work.pending.end()) {
            continue;
        }
        const bool cancelled = found->second.cancelled;
        work.input_bytes -= found->second.input_bytes;
        work.pending.erase(found); // The callback may reuse this ID.

        PrismWorkCompletionV1 projected{
            sizeof(projected), completion->id, PRISM_WORK_FAILED_V1, -1, {}, {}};
        std::string detail;
        const auto output = std::dynamic_pointer_cast<const ModuleWorkOutput>(completion->output);
        if (cancelled ||
            (completion->error && completion->error->code == runtime::TaskErrorCode::Cancelled)) {
            projected.status = PRISM_WORK_CANCELLED_V1;
            projected.error_code = 0;
            detail = "Business preparation cancelled";
        } else if (completion->error) {
            detail = completion->error->message;
        } else if (output) {
            projected.status = output->status;
            projected.error_code = output->error_code;
            projected.result = {output->result.data(), output->result.size()};
            projected.detail = {output->detail.data(), output->detail.size()};
        } else {
            detail = "Business preparation returned an invalid output";
        }
        if (!detail.empty()) {
            projected.detail = {detail.data(), detail.size()};
        }
        ++processed;
        module_.Api().on_work_completed(instance_, &projected);
    }
    return processed;
}
} // namespace prism::sdk
