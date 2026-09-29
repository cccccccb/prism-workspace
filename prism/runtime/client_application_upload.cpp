#include "client_application_p.hpp"

namespace prism::sdk {

void ClientApplication::Impl::DropImage(contracts::ResourceId id)
{
    const runtime::ImageVersion version{id, resources.Generation(id)};
    // A failed initial Open has already joined its worker. Its old command
    // queue will be discarded after this cleanup, so no release can be
    // consumed there. Before the first Open, keep releases ordered behind
    // any queued registrations for the future worker.
    const bool retired_bridge = worker_generation && !render_owner;
    if (version.generation && !closed && !failed && !retired_bridge) {
        runtime::RenderCommand command(runtime::ReleaseImageCommand{version});
        if (bridge->render_commands.TryPush(std::move(command)) ==
            runtime::QueuePushResult::Accepted) {
            pending_release_versions.emplace(id.value, version.generation);
        } else {
            bridge->terminal.Fail(runtime::TerminalReason::CommandQueueFailure);
            failed = true;
        }
    }

    uploaded_image_versions.erase(id.value);
    commands.UnregisterImage(id);
    registered_images.erase(id.value);
    resources.Release(id);
}

bool ClientApplication::Impl::RegisterImage(contracts::ResourceId id)
{
    if (registered_images.contains(id.value)) {
        return true;
    }

    auto image = resources.Retain(id);
    if (!image) {
        return false;
    }
    EnsureUiWorkBudget();

    const runtime::ImageVersion version{id, resources.Generation(id)};
    if (!version.generation || !commands.RegisterImage(id, image)) {
        return false;
    }

    runtime::RenderCommand command(runtime::RegisterImageCommand{version, std::move(image)});
    if (bridge->render_commands.TryPush(std::move(command)) != runtime::QueuePushResult::Accepted) {
        bridge->terminal.Fail(runtime::TerminalReason::CommandQueueFailure);
        commands.UnregisterImage(id);
        return false;
    }

    uploaded_image_versions.erase(id.value);
    ++owner_turn_registrations;
    registered_images.insert(id.value);
    ++loaded_images;
    ++install_stats.image_registrations;
    return true;
}

} // namespace prism::sdk
