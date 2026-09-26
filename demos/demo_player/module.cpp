#include "prism/contracts/app_module.h"
#include "prism/app/module_support.hpp"
#include <array>
#include <new>
#include <string>
#include <string_view>

namespace {
struct Player {
    const PrismHostApiV1* host;
    bool playing{};
    double progress{0.42};
    unsigned track{};
    static constexpr std::array<std::string_view,3> tracks{
        "Hotel California - Eagles", "Bohemian Rhapsody - Queen", "Starboy - The Weeknd"};
    bool Text(std::string_view key, std::string_view value) {
        PrismValueV1 binding{};
        binding.kind = PRISM_VALUE_STRING_V1;
        binding.as.string = {value.data(), value.size()};
        return host->set_binding(host->context, {key.data(), key.size()}, binding) == 0;
    }
    bool Progress() {
        const auto text = "Progress: " + std::to_string(static_cast<int>(progress * 100)) + "%";
        return Text("playback_progress_text", text) && prism::app::Number(host,"playback_progress",progress);
    }
    bool Schedule() { return host->schedule_tick(host->context, 500000000ULL) == 0; }
};
void* Create(const PrismAppInitV1* init) noexcept {
    if (!init || init->struct_size < sizeof(*init) || init->abi_version != PRISM_APP_ABI_V1 ||
        !init->host || init->host->struct_size < sizeof(PrismHostApiV1) ||
        init->host->abi_version != PRISM_APP_ABI_V1 || !init->host->set_binding ||
        !init->host->backend_ready || !init->host->schedule_tick) return nullptr;
    Player* player = nullptr;
    try {
        player = new Player{init->host};
        if (!player->Text("track_title", "Hotel California - Eagles") ||
            !player->Text("play_state_icon", "Play") || !player->Progress() || !player->Schedule() ||
            !player->Text("playback_icon", "play") ||
            init->host->backend_ready(init->host->context) != 0) {
            delete player; return nullptr;
        }
        return player;
    } catch (...) { delete player; return nullptr; }
}
void Destroy(void* instance) noexcept { delete static_cast<Player*>(instance); }
void Action(void* instance, PrismStringViewV1 text) noexcept {
    try {
        auto& player = *static_cast<Player*>(instance);
        const std::string_view action(text.data, text.size);
        if (action == "player:toggle") {
            player.playing = !player.playing;
            player.Text("play_state_icon", player.playing ? "Pause" : "Play");
            player.Text("playback_icon", player.playing ? "pause" : "play");
        } else if (action == "nav:favorites") {
            player.track=2;
            player.Text("track_title", "Starboy - The Weeknd"); player.progress = 0; player.Progress();
        } else if (action == "nav:library") {
            player.track=1;
            player.Text("track_title", "Bohemian Rhapsody - Queen"); player.progress = 0.15; player.Progress();
        } else if (action == "player:next" || action == "player:previous") {
            player.track=(player.track+(action=="player:next"?1:2))%player.tracks.size();
            player.Text("track_title",player.tracks[player.track]); player.progress=0; player.Progress();
        }
    } catch (...) {} // C ABI cannot unwind into the host.
}
void Tick(void* instance, uint64_t) noexcept {
    try {
        auto& player = *static_cast<Player*>(instance);
        if (player.playing) player.progress = player.progress + 0.02 > 1.0 ? 0 : player.progress + 0.02;
        player.Progress();
        player.Schedule();
    } catch (...) {}
}
const PrismAppModuleV1 api{sizeof(api), PRISM_APP_ABI_V1, Create, Destroy, Action, Tick, nullptr, nullptr, nullptr};
}
extern "C" PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1() { return &api; }
