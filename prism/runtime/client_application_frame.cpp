#include "client_application_p.hpp"
#include <limits>
#include <set>
#include <variant>

namespace prism::sdk {
namespace {
std::shared_ptr<const std::vector<runtime::ImageVersion>>
CollectImageUses(const contracts::DisplayList &list, const runtime::ImageResources &resources)
{
    std::set<std::uint64_t> seen;
    std::vector<runtime::ImageVersion> uses;
    for (const auto &command : list.commands) {
        const auto *image = std::get_if<contracts::DrawImage>(&command);
        if (!image || !seen.insert(image->image.value).second) {
            continue;
        }

        const auto generation = resources.Generation(image->image);
        if (!generation) {
            throw std::runtime_error("Visible image has no live resource generation");
        }
        uses.push_back({image->image, generation});
    }
    return std::make_shared<const std::vector<runtime::ImageVersion>>(std::move(uses));
}
} // namespace

std::shared_ptr<const runtime::FramePacket>
ClientApplication::Impl::CaptureFramePacket(bool pixels, contracts::BufferSize size, double scale,
                                            int configure_count)
{
    if (next_frame_sequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Frame sequence exhausted");
    }

    runtime::FramePacket packet;
    packet.sequence = ++next_frame_sequence;
    packet.ui = installed_ui;
    packet.scene_revision = scene->TransactionRevision();
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
                last_image_uses = CollectImageUses(*last_list, resources);
            }
        }
        if (!last_list) {
            throw std::runtime_error("Scene has no pixel display list");
        }

        packet.pixels_revision = scene->PixelsRevision();
    }

    // Even State-only packets retain the current pixels. A later forced redraw
    // can consume this same immutable view without returning to the live Scene.
    packet.display_list = last_list;
    packet.image_uses = last_image_uses;
    packet.surface_effects = scene->SurfaceEffects();
    packet.input_regions = scene->InputRegions();
    return std::make_shared<const runtime::FramePacket>(std::move(packet));
}

void ClientApplication::Impl::PublishFramePacket()
{
    if (!scene || !ui_configure_count) {
        return;
    }

    const auto metrics = ui_metrics;
    const auto size = metrics.buffer_size;
    const auto configure_count = ui_configure_count;
    const auto theme_generation = theme ? theme->generation : 0;
    const auto resource_epoch = commands.ResourceEpoch();
    const bool geometry_changed =
        !queued_frame || queued_frame->configure_count != configure_count ||
        queued_frame->buffer_size.width != size.width ||
        queued_frame->buffer_size.height != size.height || queued_frame->scale != metrics.scale ||
        queued_frame->ui != installed_ui;
    const bool pixels = force_frame_capture || !last_list || !ui_submitted_frame ||
                        ui_submitted_frame->buffer_size.width != size.width ||
                        ui_submitted_frame->buffer_size.height != size.height ||
                        ui_submitted_frame->scale != metrics.scale ||
                        ui_submitted_frame->ui != installed_ui ||
                        ui_submitted_frame->pixels_revision != scene->PixelsRevision();
    if (!geometry_changed && !force_frame_capture &&
        queued_frame->scene_revision == scene->TransactionRevision() &&
        queued_frame->pixels_revision == scene->PixelsRevision() &&
        queued_frame->theme_generation == theme_generation &&
        queued_frame->resource_epoch == resource_epoch) {
        return;
    }

    auto next = CaptureFramePacket(pixels, size, metrics.scale, configure_count);
    runtime::RenderCommand command(runtime::FrameCommand{next});
    const auto result =
        bridge->render_commands.TryPushLatest(std::move(command), runtime::ReplaceFrameTail);
    if (result != runtime::QueuePushResult::Accepted &&
        result != runtime::QueuePushResult::Replaced) {
        bridge->terminal.Fail(runtime::TerminalReason::CommandQueueFailure);
        throw std::runtime_error("Render command queue unavailable");
    }

    queued_frame = std::move(next);
    force_frame_capture = false;
}

} // namespace prism::sdk
