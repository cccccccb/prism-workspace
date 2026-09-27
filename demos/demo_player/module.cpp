#include "prism/app/module_support.hpp"
#include "prism/contracts/app_module.h"
#include <array>
#include <new>
#include <string>
#include <string_view>

namespace {
struct Player {
    const PrismHostApiV1 *host;
    bool playing{};
    double progress{0.42};
    unsigned track{};

    struct Track {
        std::string_view title, artist;
        unsigned duration;
    };

    static constexpr std::array<Track, 3> tracks{{{"Hotel California", "Eagles", 391},
                                                  {"Bohemian Rhapsody", "Queen", 354},
                                                  {"Starboy", "The Weeknd", 230}}};

    bool Text(std::string_view key, std::string_view value)
    {
        PrismValueV1 binding{};
        binding.kind = PRISM_VALUE_STRING_V1;
        binding.as.string = {value.data(), value.size()};
        return host->set_binding(host->context, {key.data(), key.size()}, binding) == 0;
    }

    static std::string Time(unsigned seconds)
    {
        const auto remainder = seconds % 60;
        return std::to_string(seconds / 60) + ":" + (remainder < 10 ? "0" : "") +
               std::to_string(remainder);
    }

    bool Progress()
    {
        const auto duration = tracks[track].duration;
        return Text("playback_elapsed", Time(static_cast<unsigned>(progress * duration))) &&
               Text("playback_duration", Time(duration)) &&
               prism::app::Number(host, "playback_progress", progress);
    }

    bool TrackTitle()
    {
        return Text("track_title", tracks[track].title) &&
               Text("track_artist", tracks[track].artist);
    }

    bool Schedule()
    {
        return host->schedule_tick(host->context, 500000000ULL) == 0;
    }
};

void *Create(const PrismAppInitV1 *init) noexcept
{
    if (!prism::app::ValidHost(init)) {
        return nullptr;
    }

    Player *player = nullptr;
    try {
        player = new Player{init->host};
        if (!player->TrackTitle() || !player->Progress() ||
            !player->Text("playback_icon", "play") ||
            init->host->backend_ready(init->host->context) != 0) {
            delete player;
            return nullptr;
        }
        return player;
    } catch (...) {
        delete player;
        return nullptr;
    }
}

void Destroy(void *instance) noexcept
{
    delete static_cast<Player *>(instance);
}

void Action(void *instance, PrismStringViewV1 text) noexcept
{
    try {
        auto &player = *static_cast<Player *>(instance);
        const std::string_view action(text.data, text.size);

        if (action == "player:toggle") {
            player.playing = !player.playing;
            player.Text("playback_icon", player.playing ? "pause" : "play");
            if (player.playing) {
                player.Schedule();
            }
        } else if (action == "nav:favorites") {
            player.track = 2;
            player.TrackTitle();
            player.progress = 0;
            player.Progress();
        } else if (action == "nav:library") {
            player.track = 1;
            player.TrackTitle();
            player.progress = 0.15;
            player.Progress();
        } else if (action == "player:next" || action == "player:previous") {
            player.track =
                (player.track + (action == "player:next" ? 1 : 2)) % player.tracks.size();
            player.TrackTitle();
            player.progress = 0;
            player.Progress();
        }
    } catch (...) {
    } // C ABI cannot unwind into the host.
}

void Tick(void *instance, uint64_t) noexcept
{
    try {
        auto &player = *static_cast<Player *>(instance);
        // Consume one already queued callback after pause, then stay asleep.
        if (!player.playing) {
            return;
        }

        player.progress = player.progress + 0.02 > 1.0 ? 0 : player.progress + 0.02;
        player.Progress();
        player.Schedule();
    } catch (...) {
    }
}

const PrismAppModuleV1 api{sizeof(api), PRISM_APP_ABI_V1, Create,  Destroy, Action,
                           Tick,        nullptr,          nullptr, nullptr};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &api;
}
