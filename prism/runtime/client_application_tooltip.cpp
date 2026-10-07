#include "client_application_p.hpp"

#include <limits>

namespace prism::sdk {
void ClientApplication::Impl::ReconcileTooltip()
{
    if (!scene || closed || failed || close_accept_queued) {
        return;
    }
    if (scene->ReconcileTooltip(scene->AnimationNowNs())) {
        InvalidateQueuedFrame();
        QueueRenderUpdate(true);
    }
}

int ClientApplication::Impl::TooltipTimeoutMs(int timeout_ms) const noexcept
{
    if (!scene || closed || failed || close_accept_queued) {
        return timeout_ms;
    }
    const auto deadline = scene->NextTooltipDeadlineNs();
    if (!deadline) {
        return timeout_ms;
    }

    const auto now = scene->AnimationNowNs();
    const auto remaining = *deadline > now ? *deadline - now : 0;
    const auto milliseconds = remaining / 1'000'000 + (remaining % 1'000'000 != 0);
    const int wait = static_cast<int>(
        std::min(milliseconds, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
    return timeout_ms < 0 ? wait : std::min(timeout_ms, wait);
}
} // namespace prism::sdk
