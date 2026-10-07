#include "prism/runtime/owner_feedback_session.hpp"

#include <limits>
#include <stdexcept>

namespace prism::runtime {
namespace {
std::uint64_t DeadlineAfter(std::uint64_t now, std::uint64_t duration) noexcept
{
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return duration > maximum - now ? maximum : now + duration;
}
} // namespace

void OwnerFeedbackSession::Begin(std::uint64_t generation, std::uint64_t duration_ns)
{
    if (!generation) {
        throw std::invalid_argument("Feedback generation must be nonzero");
    }

    generation_ = generation;
    remaining_ns_ = duration_ns;
    persistent_ = !duration_ns;
    adopted_ = false;
    paused_ = false;
    deadline_.reset();
}

void OwnerFeedbackSession::Clear() noexcept
{
    generation_ = 0;
    remaining_ns_ = 0;
    persistent_ = false;
    adopted_ = false;
    paused_ = false;
    deadline_.reset();
}

bool OwnerFeedbackSession::Adopt(std::uint64_t generation, std::uint64_t now_ns,
                                 bool paused) noexcept
{
    if (!generation || generation != generation_ || adopted_) {
        return false;
    }

    adopted_ = true;
    paused_ = paused;
    if (!persistent_ && !paused_) {
        deadline_ = DeadlineAfter(now_ns, remaining_ns_);
    }
    return true;
}

void OwnerFeedbackSession::Pause(std::uint64_t now_ns) noexcept
{
    if (!generation_ || paused_) {
        return;
    }

    if (deadline_) {
        remaining_ns_ = now_ns < *deadline_ ? *deadline_ - now_ns : 0;
    }
    deadline_.reset();
    paused_ = true;
}

void OwnerFeedbackSession::Resume(std::uint64_t now_ns) noexcept
{
    if (!generation_ || !paused_) {
        return;
    }

    paused_ = false;
    if (adopted_ && !persistent_) {
        deadline_ = DeadlineAfter(now_ns, remaining_ns_);
    }
}

std::optional<std::uint64_t> OwnerFeedbackSession::Deadline() const noexcept
{
    return deadline_;
}

bool OwnerFeedbackSession::Expired(std::uint64_t now_ns) const noexcept
{
    return adopted_ && !paused_ && deadline_ && now_ns >= *deadline_;
}

bool OwnerFeedbackSession::Adopted() const noexcept
{
    return adopted_;
}

std::uint64_t OwnerFeedbackSession::Generation() const noexcept
{
    return generation_;
}
} // namespace prism::runtime
