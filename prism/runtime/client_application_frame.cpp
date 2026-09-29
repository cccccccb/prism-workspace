#include "client_application_p.hpp"

namespace prism::sdk {

std::shared_ptr<const runtime::FramePacket>
ClientApplication::Impl::CaptureFramePacket(bool pixels, contracts::BufferSize size, double scale,
                                            int configure_count)
{
    runtime::FramePacket packet;
    packet.ui = installed_ui;
    packet.pixels_revision = scene->PixelsRevision();
    packet.theme_generation = theme ? theme->generation : 0;
    packet.resource_epoch = commands.ResourceEpoch();
    packet.configure_count = configure_count;
    packet.buffer_size = size;
    packet.scale = scale;

    if (pixels) {
        const auto dirty = scene->PendingDirty();
        if (!last_list || runtime::Has(dirty, runtime::Dirty::Layout) ||
            runtime::Has(dirty, runtime::Dirty::Paint)) {
            std::optional<contracts::DisplayList> next;
            {
                FirstCallTimer timer(startup_stats.first_submit_build_us, submit_build_sampled);
                next = scene->Build(contracts::WindowId{1});
            }
            if (next) {
                last_list = std::make_shared<const contracts::DisplayList>(std::move(*next));
            }
        }
        if (!last_list) {
            throw std::runtime_error("Scene has no pixel display list");
        }

        packet.pixels_revision = scene->PixelsRevision();
        packet.display_list = last_list;
    }

    packet.surface_effects = scene->SurfaceEffects();
    packet.input_regions = scene->InputRegions();
    return std::make_shared<const runtime::FramePacket>(std::move(packet));
}

} // namespace prism::sdk
