#include "app_host_p.hpp"
#include <algorithm>

namespace prism::sdk {
namespace host_detail {
PumpProcessingTimer::PumpProcessingTimer(HostStartupStats &stats, const ClientApplication &frontend,
                                         const std::uint64_t &host_wait_ns)
    : stats_(stats), frontend_(frontend), host_wait_ns_(host_wait_ns), start_ns_(MonotonicNs()),
      wait_ns_(frontend.WaitDurationNs() + host_wait_ns)
{
}

PumpProcessingTimer::~PumpProcessingTimer()
{
    const auto elapsed = MonotonicNs() - start_ns_;
    const auto waited = frontend_.WaitDurationNs() + host_wait_ns_ - wait_ns_;
    stats_.pump_processing_last_us = (elapsed - std::min(elapsed, waited)) / 1000;
    ++stats_.pump_processing_count;
    stats_.pump_processing_total_us += stats_.pump_processing_last_us;
    stats_.pump_processing_max_us =
        std::max(stats_.pump_processing_max_us, stats_.pump_processing_last_us);
}
} // namespace host_detail

HostStartupStats AppHost::GetStartupStats() const
{
    return impl_->startup;
}

void AppHost::Impl::ObserveStartup()
{
    const auto client = frontend->GetStartupStats();
    startup.egl_init_us = client.egl_init_us;
    startup.ganesh_init_us = client.ganesh_init_us;
    startup.first_submit_build_us = client.first_submit_build_us;
    startup.first_render_us = client.first_render_us;
    startup.first_swap_us = client.first_swap_us;

    const auto now = MonotonicNs();
    if (ui.preview_submitted && !startup.preview_submitted_ns) {
        startup.preview_submitted_ns = now;
    }
    if (ui.preview_presented && !startup.preview_presented_ns) {
        startup.preview_presented_ns = now;
    }
    if (ui.master_submitted && !startup.master_submitted_ns) {
        startup.master_submitted_ns = now;
    }
    if (ui.master_presented && !startup.master_presented_ns) {
        startup.master_presented_ns = now;
    }
    if (ready && !startup.backend_ready_ns) {
        startup.backend_ready_ns = now;
    }
    if (ui.master_presented && !startup.deferred_complete_ns && installed_plan) {
        const auto deferred =
            std::count_if(installed_plan->components.begin(), installed_plan->components.end(),
                          [](const runtime::LoadUnit &unit) {
                              return unit.phase == runtime::LoadPhase::Deferred;
                          });
        if (installed_regions.size() == static_cast<std::size_t>(deferred)) {
            startup.deferred_complete_ns = now;
        }
    }
}
} // namespace prism::sdk
