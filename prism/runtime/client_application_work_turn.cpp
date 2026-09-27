#include "client_application_p.hpp"
#include <algorithm>

namespace prism::sdk {
void ClientApplication::Impl::ResetUiWorkBudget() noexcept
{
    ui_work_turn_started = false;
    owner_turn_uploads = 0;
    owner_turn_upload_bytes = 0;
    owner_turn_nodes = 0;
    owner_turn_requests = 0;
    owner_turn_registrations = 0;
    owner_turn_image_ready = 0;
}

void ClientApplication::Impl::EnsureUiWorkBudget()
{
    if (!ui_work_turn_started) {
        ui_work_turn_started = true;
        owner_turn_deadline = std::chrono::steady_clock::now() + config.install_limits.cpu_per_turn;
        ++install_stats.turns;
    }
}

void ClientApplication::Impl::RecordUiWorkBudget() noexcept
{
    if (ui_work_turn_started) {
        const auto start = owner_turn_deadline - config.install_limits.cpu_per_turn;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        install_stats.max_turn_us =
            std::max(install_stats.max_turn_us, static_cast<std::uint64_t>(elapsed.count()));
    }
}

void ClientApplication::BeginUiWorkTurn()
{
    auto &app = *impl_;
    if (app.ui_work_turn_explicit) {
        throw std::logic_error("A UI owner work turn is already active");
    }
    app.ResetUiWorkBudget();
    app.install_advanced_since_pump = false;
    app.ui_work_turn_explicit = true;
}

bool ClientApplication::EndUiWorkTurn() noexcept
{
    auto &app = *impl_;
    if (!app.ui_work_turn_explicit) {
        return false;
    }
    app.RecordUiWorkBudget();
    app.ResetUiWorkBudget();
    app.ui_work_turn_explicit = false;
    app.install_advanced_since_pump = false;
    return true;
}
} // namespace prism::sdk
