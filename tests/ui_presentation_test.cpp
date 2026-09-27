#include "prism/sdk/ui_presentation.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {
using prism::platform::PixelPresentation;
using prism::platform::PresentationOutcome;
using prism::runtime::UiLoadId;
using prism::sdk::UiPresentationTracker;

void DistinctUiGenerationsAndOutOfOrderFeedback()
{
    UiPresentationTracker tracker;
    const UiLoadId preview{11, 1};
    const UiLoadId master{11, 2};
    tracker.Install(preview);
    assert(tracker.Submit(preview, {1}, true));
    assert(tracker.Submit(preview, {2}, true));
    tracker.Install(master);
    assert(tracker.Get(master).installed && !tracker.Get(master).submitted);
    assert(tracker.Submit(master, {3}, true));
    assert(tracker.Submit(master, {4}, true));

    // The old frame can arrive after Master has already been installed and
    // submitted. Its feedback still proves only Preview's actual presentation.
    assert(tracker.Present({{2}, PresentationOutcome::Presented}) == preview);
    assert(tracker.Get(preview).presented);
    assert(!tracker.Get(master).presented);
    assert(tracker.Get(master).last_presented_submission == 0);
    assert(tracker.Present({{3}, PresentationOutcome::Discarded}) == master);
    assert(tracker.Get(master).discarded_count == 1 && !tracker.Get(master).presented);
    assert(tracker.Get(master).last_presented_submission == 0);
    assert(tracker.Present({{4}, PresentationOutcome::Presented}) == master);
    assert(tracker.Present({{1}, PresentationOutcome::Presented}) == preview);

    const auto preview_state = tracker.Get(preview);
    const auto master_state = tracker.Get(master);
    assert(preview_state.first_submission == 1 && preview_state.last_submission == 2);
    assert(preview_state.last_presented_submission == 2);
    assert(preview_state.submitted_count == 2 && preview_state.presented_count == 2);
    assert(master_state.first_submission == 3 && master_state.last_submission == 4);
    assert(master_state.last_presented_submission == 4 && master_state.presented_count == 1);
    assert(master_state.submitted && master_state.presented);
    assert(!tracker.Present({{4}, PresentationOutcome::Presented}).owner);
    assert(tracker.Get(master).presented_count == 1);
}

void ExactSubmissionIdentity()
{
    UiPresentationTracker tracker;
    const UiLoadId current{22, 1};
    tracker.Install(current);
    assert(!tracker.Submit({}, {1}, true));
    assert(!tracker.Submit(current, {}, true));
    assert(tracker.Submit(current, {9}, true));
    assert(!tracker.Submit(current, {9}, true));
    assert(!tracker.Submit(current, {8}, true));
    assert(!tracker.Present({{8}, PresentationOutcome::Presented}).owner);
    assert(!tracker.Get(current).presented);
    assert(tracker.Present({{9}, PresentationOutcome::Presented}) == current);
    assert(tracker.Get(current).submitted_count == 1);
    assert(tracker.Get(current).presented_count == 1);
    const auto foreign = tracker.Get({23, 1});
    assert((foreign.load == UiLoadId{23, 1}));
    assert(!foreign.installed && !foreign.submitted && !foreign.presented);
}

void RetainedHistoryAndClear()
{
    UiPresentationTracker tracker;
    const UiLoadId first{33, 1};
    const UiLoadId second{33, 2};
    const UiLoadId third{33, 3};
    tracker.Install(first);
    assert(tracker.Submit(first, {1}, true));
    tracker.Install(second);
    assert(tracker.Submit(second, {2}, true));
    tracker.Install(third);
    assert(!tracker.Get(first).installed);
    assert(tracker.Get(second).installed && tracker.Get(third).installed);
    assert(!tracker.Present({{1}, PresentationOutcome::Presented}).owner);
    assert(!tracker.Get(third).presented);
    assert(tracker.Present({{2}, PresentationOutcome::Presented}) == second);
    assert(tracker.Submit(third, {3}, true));

    tracker.Clear();
    assert(!tracker.Get(second).installed && !tracker.Get(third).submitted);
    assert(!tracker.Present({{3}, PresentationOutcome::Presented}).owner);
    assert(!tracker.Get(third).presented);
}

void BoundedPendingFeedback()
{
    UiPresentationTracker tracker;
    const UiLoadId load{44, 1};
    tracker.Install(load);
    for (std::uint64_t submission = 1; submission <= 8; ++submission) {
        assert(tracker.Submit(load, {submission}, true));
    }
    // Overflow is rejected before publishing any submission metadata. A
    // drained feedback makes space without altering the remaining mappings.
    assert(!tracker.Submit(load, {9}, true));
    assert(tracker.Get(load).last_submission == 8);
    assert(tracker.Get(load).submitted_count == 8);
    assert(tracker.Present({{4}, PresentationOutcome::Discarded}) == load);
    assert(tracker.Submit(load, {9}, true));
    assert(tracker.Present({{9}, PresentationOutcome::Presented}) == load);
    assert(tracker.Present({{1}, PresentationOutcome::Presented}) == load);
    assert(tracker.Get(load).last_presented_submission == 9);
    assert(tracker.Get(load).discarded_count == 1);
}

void SubmissionWithoutPresentationProtocol()
{
    UiPresentationTracker tracker;
    const UiLoadId load{55, 1};
    tracker.Install(load);
    assert(tracker.Submit(load, {1}, false));
    const auto state = tracker.Get(load);
    assert(state.installed && state.submitted && !state.presented);
    assert(state.first_submission == 1 && state.last_submission == 1);
    assert(!tracker.Present(PixelPresentation{{1}, PresentationOutcome::Presented}).owner);
    assert(!tracker.Get(load).presented);
}
} // namespace

int main()
{
    DistinctUiGenerationsAndOutOfOrderFeedback();
    ExactSubmissionIdentity();
    RetainedHistoryAndClear();
    BoundedPendingFeedback();
    SubmissionWithoutPresentationProtocol();
    std::cout << "UI submission identity, feedback provenance and bounded history passed\n";
}
