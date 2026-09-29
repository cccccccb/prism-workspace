#include "prism/runtime/render_worker_lifecycle.hpp"

#include <atomic>
#include <stdexcept>

namespace prism::runtime {
namespace {

std::atomic<std::uint64_t> next_worker_generation{1};

RenderWorkerOpenWait ToOpenWait(RenderWorkerOpenStatus status)
{
    switch (status) {
    case RenderWorkerOpenStatus::Opened:
        return RenderWorkerOpenWait::Opened;
    case RenderWorkerOpenStatus::Failed:
        return RenderWorkerOpenWait::Failed;
    case RenderWorkerOpenStatus::Cancelled:
        return RenderWorkerOpenWait::Cancelled;
    case RenderWorkerOpenStatus::Started:
        return RenderWorkerOpenWait::TimedOut;
    case RenderWorkerOpenStatus::Stale:
        return RenderWorkerOpenWait::Stale;
    }
    return RenderWorkerOpenWait::Stale;
}

} // namespace

RenderWorkerLifecycle::RenderWorkerLifecycle(TerminalSignal &terminal) noexcept
    : terminal_(terminal)
{
}

std::optional<RenderWorkerGeneration> RenderWorkerLifecycle::BeginOpen()
{
    std::lock_guard lock(mutex_);
    if (generation_.value || terminal_.StopRequested() ||
        terminal_.Reason() != TerminalReason::None) {
        return std::nullopt;
    }

    const auto value = next_worker_generation.fetch_add(1, std::memory_order_relaxed);
    if (!value) {
        throw std::overflow_error("Render worker generation exhausted");
    }

    generation_ = {value};
    open_status_ = RenderWorkerOpenStatus::Started;
    return generation_;
}

bool RenderWorkerLifecycle::CompleteOpen(RenderWorkerGeneration generation,
                                         RenderWorkerOpenOutcome outcome)
{
    {
        std::lock_guard lock(mutex_);
        if (!Matches(generation) || open_status_ != RenderWorkerOpenStatus::Started ||
            (close_requested_ && outcome == RenderWorkerOpenOutcome::Opened)) {
            return false;
        }

        switch (outcome) {
        case RenderWorkerOpenOutcome::Opened:
            open_status_ = RenderWorkerOpenStatus::Opened;
            break;
        case RenderWorkerOpenOutcome::Failed:
            open_status_ = RenderWorkerOpenStatus::Failed;
            break;
        case RenderWorkerOpenOutcome::Cancelled:
            open_status_ = RenderWorkerOpenStatus::Cancelled;
            close_requested_ = true;
            break;
        }

        if (outcome == RenderWorkerOpenOutcome::Failed) {
            terminal_.Fail(TerminalReason::OpenFailure);
        } else if (outcome == RenderWorkerOpenOutcome::Cancelled) {
            terminal_.RequestStop();
        }
    }
    changed_.notify_all();
    return true;
}

bool RenderWorkerLifecycle::CompleteClose(RenderWorkerGeneration generation)
{
    {
        std::lock_guard lock(mutex_);
        if (!Matches(generation) || close_acknowledged_) {
            return false;
        }

        close_requested_ = true;
        close_acknowledged_ = true;
        if (open_status_ == RenderWorkerOpenStatus::Started) {
            open_status_ = RenderWorkerOpenStatus::Cancelled;
        }

        terminal_.RequestStop();
    }
    changed_.notify_all();
    return true;
}

RenderWorkerOpenStatus RenderWorkerLifecycle::OpenStatus(RenderWorkerGeneration generation) const
{
    std::lock_guard lock(mutex_);
    return Matches(generation) ? open_status_ : RenderWorkerOpenStatus::Stale;
}

RenderWorkerOpenWait RenderWorkerLifecycle::WaitOpen(RenderWorkerGeneration generation,
                                                     std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    if (!Matches(generation)) {
        return RenderWorkerOpenWait::Stale;
    }

    changed_.wait_for(lock, timeout,
                      [this] { return open_status_ != RenderWorkerOpenStatus::Started; });
    return ToOpenWait(open_status_);
}

bool RenderWorkerLifecycle::RequestClose(RenderWorkerGeneration generation)
{
    {
        std::lock_guard lock(mutex_);
        if (!Matches(generation) || close_requested_ || close_acknowledged_) {
            return false;
        }

        close_requested_ = true;
        if (open_status_ == RenderWorkerOpenStatus::Started) {
            open_status_ = RenderWorkerOpenStatus::Cancelled;
        }

        terminal_.RequestStop();
    }
    changed_.notify_all();
    return true;
}

bool RenderWorkerLifecycle::CloseRequested(RenderWorkerGeneration generation) const
{
    std::lock_guard lock(mutex_);
    return Matches(generation) && (close_requested_ || terminal_.StopRequested());
}

RenderWorkerCloseWait RenderWorkerLifecycle::WaitClose(RenderWorkerGeneration generation,
                                                       std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    if (!Matches(generation)) {
        return RenderWorkerCloseWait::Stale;
    }

    if (!changed_.wait_for(lock, timeout, [this] { return close_acknowledged_; })) {
        return RenderWorkerCloseWait::TimedOut;
    }
    return RenderWorkerCloseWait::Acknowledged;
}

bool RenderWorkerLifecycle::Matches(RenderWorkerGeneration generation) const noexcept
{
    return generation.value && generation == generation_;
}

} // namespace prism::runtime
