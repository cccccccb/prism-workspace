#pragma once

#include "prism/contracts/gpu_target.hpp"
#include "prism/platform/wayland_egl_surface.hpp"
#include "prism/runtime/buffer_damage.hpp"
#include "prism/runtime/frame_packet.hpp"
#include "prism/runtime/render_bridge_types.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace prism::sdk {

// One render-worker-owned surface. GPU resources/context belong to the owner;
// pixel history and the input geometry actually submitted belong to this target.
// Clearing submission state does not destroy its WSI or the shared context.
struct ClientRenderSurfaceState {
    platform::WaylandEglSurface egl;
    runtime::UiLoadId input_ui{};
    std::shared_ptr<const runtime::InputSnapshot> input_snapshot;
    std::shared_ptr<const runtime::FramePacket> render_frame, committed_frame, prepared_frame;
    std::uint64_t committed_damage_resource_epoch{};
    runtime::BufferDamageHistory damage_history;
    std::optional<runtime::BufferDamagePlan> prepared_damage;
    contracts::GpuTargetIdentity prepared_target{};
    std::uint64_t prepared_content_area{};
    runtime::RenderBackendStats backend_stats{};
    int presented{};

    void ResetSubmission() noexcept
    {
        damage_history.Invalidate();
        prepared_damage.reset();
        prepared_target = {};
        prepared_frame.reset();
        render_frame.reset();

        input_snapshot.reset();
        input_ui = {};
        committed_frame.reset();
        committed_damage_resource_epoch = 0;
        prepared_content_area = 0;
    }
};

} // namespace prism::sdk
