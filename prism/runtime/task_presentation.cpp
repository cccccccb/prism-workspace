#include "prism/runtime/task_presentation.hpp"

#include <cmath>
#include <limits>

namespace prism::runtime {
namespace {
bool ValidScope(std::uint64_t token, std::uint64_t epoch, contracts::NodeId root) noexcept
{
    return token && epoch && root;
}

bool ValidBinding(const TaskPresentationBinding &binding) noexcept
{
    return binding.modal_epoch && binding.input_scene && binding.input_version &&
           binding.configure_count > 0 && binding.buffer_width && binding.buffer_height &&
           std::isfinite(binding.scale) && binding.scale > 0;
}

bool ValidInterruptReason(TaskPresentationInterruptReason reason) noexcept
{
    switch (reason) {
    case TaskPresentationInterruptReason::UiReplaced:
    case TaskPresentationInterruptReason::OwnerRetired:
    case TaskPresentationInterruptReason::FrontendFailed:
    case TaskPresentationInterruptReason::GeometryChanged:
    case TaskPresentationInterruptReason::ThemeChanged:
    case TaskPresentationInterruptReason::ScopeUnavailable:
    case TaskPresentationInterruptReason::Superseded:
        return true;
    }
    return false;
}
} // namespace

std::optional<TaskPresentationIdentity>
TaskPresentationSession::Begin(TaskIdentity task, UiLoadId ui, std::uint64_t modal_token,
                               std::uint64_t modal_epoch, contracts::NodeId root) noexcept
{
    if (!task.owner || !task.request || !ui.owner || !ui.generation ||
        !ValidScope(modal_token, modal_epoch, root) || IsLive() ||
        last_cycle_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    const TaskPresentationIdentity identity{task, ui, last_cycle_ + 1};
    current_ = TaskPresentationState{
        identity, TaskPresentationPhase::Opening, 1, std::nullopt, 0, std::nullopt};
    scope_ = {modal_token, modal_epoch, root};
    ever_adopted_ = false;
    first_published_sequence_ = 0;
    last_published_sequence_ = 0;
    first_terminal_sequence_ = 0;
    last_cycle_ = identity.cycle;
    return identity;
}

bool TaskPresentationSession::Matches(TaskPresentationIdentity identity) const noexcept
{
    return current_ && current_->identity == identity;
}

bool TaskPresentationSession::IsLive() const noexcept
{
    return current_ && (current_->phase == TaskPresentationPhase::Opening ||
                        current_->phase == TaskPresentationPhase::Open);
}

bool TaskPresentationSession::MatchesScope(const TaskPresentationBinding &binding) const noexcept
{
    return binding.modal_token == scope_.token && binding.modal_epoch == scope_.epoch &&
           binding.root == scope_.root;
}

void TaskPresentationSession::ClearProjection() noexcept
{
    current_->binding.reset();
    current_->adopted_sequence = 0;
    first_published_sequence_ = 0;
    last_published_sequence_ = 0;
    first_terminal_sequence_ = 0;
}

bool TaskPresentationSession::Reproject(TaskPresentationIdentity identity,
                                        std::uint64_t modal_token, std::uint64_t modal_epoch,
                                        contracts::NodeId root) noexcept
{
    if (!Matches(identity) || !IsLive() || !ValidScope(modal_token, modal_epoch, root) ||
        modal_epoch <= scope_.epoch ||
        current_->projection == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    scope_ = {modal_token, modal_epoch, root};
    ++current_->projection;
    ClearProjection();
    return true;
}

bool TaskPresentationSession::InvalidateProjection(TaskPresentationIdentity identity) noexcept
{
    if (!Matches(identity) || !IsLive() ||
        current_->projection == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    ++current_->projection;
    ClearProjection();
    return true;
}

std::optional<TaskPresentationStamp> TaskPresentationSession::Publish(
    TaskPresentationIdentity identity, const TaskPresentationBinding &binding,
    std::uint64_t frame_sequence, TaskPresentationSampleKind sample_kind) noexcept
{
    if (!Matches(identity) || current_->phase == TaskPresentationPhase::Closed ||
        !ValidBinding(binding) || !MatchesScope(binding) || !frame_sequence ||
        frame_sequence <= last_frame_sequence_ ||
        (sample_kind != TaskPresentationSampleKind::Intermediate &&
         sample_kind != TaskPresentationSampleKind::Terminal)) {
        return std::nullopt;
    }
    const bool changed = current_->binding && *current_->binding != binding;
    if (sample_kind == TaskPresentationSampleKind::Intermediate &&
        (current_->phase == TaskPresentationPhase::Open ||
         (!changed && first_terminal_sequence_))) {
        return std::nullopt;
    }
    if (changed && current_->projection == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    if (changed) {
        ++current_->projection;
        ClearProjection();
    }
    if (!current_->binding) {
        current_->binding = binding;
        first_published_sequence_ = frame_sequence;
    }
    last_published_sequence_ = frame_sequence;
    last_frame_sequence_ = frame_sequence;
    if (sample_kind == TaskPresentationSampleKind::Terminal && !first_terminal_sequence_) {
        first_terminal_sequence_ = frame_sequence;
    }

    const auto endpoint = current_->phase == TaskPresentationPhase::Closing
                              ? TaskPresentationEndpoint::Closed
                              : TaskPresentationEndpoint::Open;
    return TaskPresentationStamp{identity, current_->projection, endpoint,
                                 binding,  frame_sequence,       sample_kind};
}

bool TaskPresentationSession::Adopt(const TaskPresentationStamp &stamp) noexcept
{
    if (!Matches(stamp.identity) || current_->phase == TaskPresentationPhase::Closed ||
        !current_->binding || stamp.projection != current_->projection ||
        stamp.binding != *current_->binding || stamp.frame_sequence < first_published_sequence_ ||
        stamp.frame_sequence > last_published_sequence_ ||
        stamp.frame_sequence <= current_->adopted_sequence) {
        return false;
    }
    const auto endpoint = current_->phase == TaskPresentationPhase::Closing
                              ? TaskPresentationEndpoint::Closed
                              : TaskPresentationEndpoint::Open;
    if (stamp.endpoint != endpoint) {
        return false;
    }
    const auto sample_kind =
        first_terminal_sequence_ && stamp.frame_sequence >= first_terminal_sequence_
            ? TaskPresentationSampleKind::Terminal
            : TaskPresentationSampleKind::Intermediate;
    if (stamp.sample_kind != sample_kind || (current_->phase == TaskPresentationPhase::Open &&
                                             sample_kind != TaskPresentationSampleKind::Terminal)) {
        return false;
    }

    current_->adopted_sequence = stamp.frame_sequence;
    ever_adopted_ = true;
    if (sample_kind == TaskPresentationSampleKind::Terminal) {
        current_->phase = endpoint == TaskPresentationEndpoint::Closed
                              ? TaskPresentationPhase::Closed
                              : TaskPresentationPhase::Open;
    }
    return true;
}

bool TaskPresentationSession::BeginClose(TaskPresentationIdentity identity,
                                         std::uint64_t closed_epoch) noexcept
{
    if (!Matches(identity) || !IsLive() || closed_epoch <= scope_.epoch ||
        current_->projection == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    scope_ = {0, closed_epoch, {}};
    ++current_->projection;
    ClearProjection();
    current_->phase =
        ever_adopted_ ? TaskPresentationPhase::Closing : TaskPresentationPhase::Closed;
    return true;
}

bool TaskPresentationSession::Interrupt(TaskPresentationIdentity identity,
                                        TaskPresentationInterruptReason reason) noexcept
{
    if (!Matches(identity) || current_->phase == TaskPresentationPhase::Closed ||
        !ValidInterruptReason(reason)) {
        return false;
    }

    ClearProjection();
    scope_.token = 0;
    scope_.root = {};
    current_->phase = TaskPresentationPhase::Closed;
    current_->interruption = reason;
    return true;
}

std::optional<TaskPresentationState> TaskPresentationSession::Current() const noexcept
{
    return current_;
}

} // namespace prism::runtime
