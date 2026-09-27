#pragma once

#include "prism/platform/presentation.hpp"
#include "prism/runtime/ui_load.hpp"
#include <array>
#include <cstdint>

namespace prism::sdk {
struct UiPresentationState {
    runtime::UiLoadId load{};
    bool installed{}, submitted{}, presented{};
    std::uint64_t first_submission{}, last_submission{}, last_presented_submission{};
    std::uint64_t submitted_count{}, presented_count{}, discarded_count{};
};

// Owner-thread value state, independent of Wayland and GPU objects. Only the
// installed UI and its predecessor are retained. Feedback is matched exactly,
// never attributed to whichever Scene happens to be current on receipt.
class UiPresentationTracker {
public:
    void Install(runtime::UiLoadId load) noexcept
    {
        if (!load.owner || !load.generation || current_.load == load) {
            return;
        }
        prior_ = current_;
        current_ = {load, true};
        for (auto &pending : pending_) {
            if (pending.load != current_.load && pending.load != prior_.load) {
                pending = {};
            }
        }
    }

    bool Submit(runtime::UiLoadId load, platform::PixelSubmissionId submission,
                bool feedback_expected) noexcept
    {
        auto *state = Find(load);
        if (!state || !submission || submission.value <= last_submission_) {
            return false;
        }
        Pending *slot = nullptr;
        if (feedback_expected) {
            for (auto &pending : pending_) {
                if (!pending.submission) {
                    slot = &pending;
                    break;
                }
            }
            if (!slot) {
                return false;
            }
        }

        if (!state->submitted) {
            state->first_submission = submission.value;
        }
        state->submitted = true;
        state->last_submission = submission.value;
        ++state->submitted_count;
        last_submission_ = submission.value;
        if (slot) {
            *slot = {load, submission};
        }
        return true;
    }

    runtime::UiLoadId Present(platform::PixelPresentation event) noexcept
    {
        for (auto &pending : pending_) {
            if (!event.submission || pending.submission != event.submission) {
                continue;
            }
            const auto load = pending.load;
            pending = {};
            auto *state = Find(load);
            if (!state) {
                return {};
            }
            if (event.outcome == platform::PresentationOutcome::Presented) {
                state->presented = true;
                if (event.submission.value > state->last_presented_submission) {
                    state->last_presented_submission = event.submission.value;
                }
                ++state->presented_count;
            } else {
                ++state->discarded_count;
            }
            return load;
        }
        return {};
    }

    UiPresentationState Get(runtime::UiLoadId load) const noexcept
    {
        if (current_.installed && current_.load == load) {
            return current_;
        }
        if (prior_.installed && prior_.load == load) {
            return prior_;
        }
        return {load};
    }

    void Clear() noexcept
    {
        current_ = {};
        prior_ = {};
        pending_ = {};
        last_submission_ = 0;
    }

private:
    struct Pending {
        runtime::UiLoadId load{};
        platform::PixelSubmissionId submission{};
    };

    UiPresentationState *Find(runtime::UiLoadId load) noexcept
    {
        if (current_.installed && current_.load == load) {
            return &current_;
        }
        if (prior_.installed && prior_.load == load) {
            return &prior_;
        }
        return nullptr;
    }

    UiPresentationState current_, prior_;
    std::array<Pending, 8> pending_{};
    std::uint64_t last_submission_{};
};
} // namespace prism::sdk
