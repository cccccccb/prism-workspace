#pragma once

#include "prism/contracts/display_list.hpp"
#include "prism/runtime/task_presentation.hpp"

#include <cstdint>
#include <vector>

namespace prism::runtime {

// Source association only. No Scene node, input identity, modal token or action
// escapes into the retained drawing value. Projection/sequence identify the
// adopted source; refresh does not make an unadopted candidate authoritative.
struct TaskPaintSource {
    TaskPresentationIdentity identity;
    std::uint64_t projection{};
    std::uint64_t frame_sequence{};
    int configure_count{};
    contracts::BufferSize buffer_size{};
    double scale{1.0};
    std::uint64_t theme_generation{};
    std::uint64_t resource_epoch{};
};

// Independently owned subtree commands, including ancestor drawing scopes.
// Image/backdrop leases are not implemented. Glyphs use the application's
// fixed font lifetime; changing UI/geometry/theme/resource state retires this
// value. Publishing it through shared_ptr<const> makes it readonly.
struct TaskPaintFragment {
    TaskPaintSource source;
    std::vector<contracts::DrawCommand> commands;
};

// A pending refresh may still close from the last adopted source in the same
// cycle. Source projection/sequence are deliberately not environment fences.
bool MatchesTaskPaintEnvironment(const TaskPaintSource &, const TaskPaintSource &) noexcept;

// Value composition for a fixed-geometry exit sample. The caller supplies the
// latest body and an eligible retained fragment, never its former whole frame.
// The resulting list enters the renderer's existing old/new damage comparison.
// Zero preserves the body commands; one preserves the unmodulated composition.
// Invalid opacity throws without changing either input value.
contracts::DisplayList ComposeTaskPaint(const contracts::DisplayList &body,
                                        const TaskPaintFragment &paint, double opacity = 1);

} // namespace prism::runtime
