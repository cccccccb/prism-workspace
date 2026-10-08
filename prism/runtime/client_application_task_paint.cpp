#include "client_application_p.hpp"
#include "prism/runtime/owner_task_panel.hpp"

#include <variant>

namespace prism::sdk {
namespace {
runtime::TaskPaintSource PaintSource(const runtime::FramePacket &packet)
{
    const auto &stamp = *packet.task_presentation;
    return {stamp.identity,     stamp.projection, packet.sequence,         packet.configure_count,
            packet.buffer_size, packet.scale,     packet.theme_generation, packet.resource_epoch};
}

bool UsesFixedFontOnly(const std::vector<contracts::DrawCommand> &commands,
                       contracts::ResourceId font)
{
    for (const auto &command : commands) {
        if (std::holds_alternative<contracts::DrawImage>(command)) {
            return false;
        }
        if (const auto *glyphs = std::get_if<contracts::DrawGlyphRun>(&command);
            glyphs && glyphs->font != font) {
            return false;
        }
    }
    return true;
}
} // namespace

std::optional<runtime::TaskPaintFragment>
ClientApplication::Impl::CaptureOwnerTaskPaint(const runtime::FramePacket &packet)
{
    const auto state = owner_task_presentation.Current();
    if (!state || state->phase == runtime::TaskPresentationPhase::Closed) {
        owner_task_paint.reset();
        return std::nullopt;
    }

    const runtime::TaskPaintSource source{
        state->identity,    state->projection, packet.sequence,         packet.configure_count,
        packet.buffer_size, packet.scale,      packet.theme_generation, packet.resource_epoch};
    if (owner_task_paint &&
        !runtime::MatchesTaskPaintEnvironment(owner_task_paint->source, source)) {
        owner_task_paint.reset();
    }
    if (state->phase == runtime::TaskPresentationPhase::Closing || !IsStandardOwnerTask()) {
        return std::nullopt;
    }

    // Export exactly the final resolved drawing sample. Composite-only changes
    // can introduce backdrop dependencies without changing pixels_revision,
    // so that revision alone is insufficient to reuse an earlier candidate.
    try {
        auto exported = scene->CaptureTaskPaint(owner_task_scope->root);
        if (!exported || !UsesFixedFontOnly(exported->commands, shaper.FontId())) {
            return std::nullopt;
        }
        runtime::TaskPaintFragment paint;
        paint.source = source;
        paint.commands = std::move(exported->commands);
        return paint;
    } catch (...) {
        // Retained paint is optional. Unsupported/failed capture cannot keep
        // the business scope alive or obstruct terminal/input retirement.
        return std::nullopt;
    }
}

void ClientApplication::Impl::AdoptOwnerTaskPaint(const runtime::FramePacket &packet)
{
    const auto state = owner_task_presentation.Current();
    if (!state || !packet.task_presentation ||
        state->identity != packet.task_presentation->identity ||
        state->adopted_sequence != packet.sequence) {
        return;
    }
    if (state->phase == runtime::TaskPresentationPhase::Closed) {
        owner_task_paint.reset();
        return;
    }
    if (packet.task_presentation->endpoint == runtime::TaskPresentationEndpoint::Closed) {
        return; // Closing samples do not replace their retained unmodulated source.
    }

    // A newly adopted unsupported panel invalidates its earlier supported
    // source. Pending, unadopted refreshes do not enter this method.
    const auto &candidate = packet.task_paint_candidate;
    const auto source = PaintSource(packet);
    if (!candidate || !runtime::MatchesTaskPaintEnvironment(candidate->source, source) ||
        candidate->source.projection != source.projection ||
        candidate->source.frame_sequence != source.frame_sequence ||
        !UsesFixedFontOnly(candidate->commands, shaper.FontId())) {
        owner_task_paint.reset();
        return;
    }
    owner_task_paint = candidate;
}

std::shared_ptr<const runtime::TaskPaintFragment>
ClientApplication::Impl::ClosingOwnerTaskPaint() const
{
    const auto state = owner_task_presentation.Current();
    if (!owner_task_paint || !state || state->phase != runtime::TaskPresentationPhase::Closing ||
        closed || failed || owner_tasks_retired || state->identity.ui != installed_ui) {
        return {};
    }

    const runtime::TaskPaintSource current{state->identity,
                                           state->projection,
                                           0,
                                           ui_configure_count,
                                           ui_metrics.buffer_size,
                                           ui_metrics.scale,
                                           theme ? theme->generation : 0,
                                           commands.ResourceEpoch()};
    return runtime::MatchesTaskPaintEnvironment(owner_task_paint->source, current)
               ? owner_task_paint
               : nullptr;
}

} // namespace prism::sdk
