#pragma once

#include "prism/runtime/frame_packet.hpp"

#include <cmath>
#include <cstdint>

namespace prism::sdk {
// The caller supplies successful root metadata and an actual successful pixel
// baseline. A staged candidate cannot substitute for the latter. Same-size
// parent configure ACKs may keep older pixels, so only metadata names the
// current parent configure; native child identity is checked independently.
inline bool CanEnablePopupBackdrop(const runtime::FramePacket &metadata,
                                   const runtime::FramePacket *pixels,
                                   const runtime::PopupSurfaceIdentity &adopted_child,
                                   runtime::UiLoadId ui, int parent_configure) noexcept
{
    const auto &excluded = metadata.popup_surface_excluded;
    if (!ui.owner || !ui.generation || !pixels || metadata.ui != ui || pixels->ui != ui ||
        parent_configure <= 0 || metadata.configure_count != parent_configure ||
        !metadata.popup_surface_request ||
        metadata.popup_surface_request->parent_configure_generation !=
            static_cast<std::uint64_t>(parent_configure) ||
        !adopted_child.worker || !adopted_child.target ||
        adopted_child.target != adopted_child.lifetime || !adopted_child.configure_generation ||
        !adopted_child.submission_sequence || !excluded.submission_sequence ||
        excluded.submission_sequence > adopted_child.submission_sequence ||
        excluded.worker != adopted_child.worker || excluded.target != adopted_child.target ||
        excluded.lifetime != adopted_child.lifetime ||
        excluded.configure_generation != adopted_child.configure_generation) {
        return false;
    }

    return metadata.display_list && pixels->display_list &&
           metadata.display_list == pixels->display_list &&
           metadata.resource_epoch == pixels->resource_epoch && metadata.buffer_size.width > 0 &&
           metadata.buffer_size.height > 0 &&
           metadata.buffer_size.width == pixels->buffer_size.width &&
           metadata.buffer_size.height == pixels->buffer_size.height &&
           std::isfinite(metadata.scale) && metadata.scale > 0 && metadata.scale == pixels->scale;
}
} // namespace prism::sdk
