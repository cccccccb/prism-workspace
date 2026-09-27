#include "player.hpp"
#include "prism/app/module_support.hpp"
#include <exception>
#include <nlohmann/json.hpp>
#include <utility>

namespace prism::music {
Player::Player(const PrismHostApiV1 *host, std::string assets_root)
    : host_(host), assets_root_(std::move(assets_root))
{
}

bool Player::Text(std::string_view key, std::string_view text)
{
    return prism::app::Text(host_, key, text);
}

bool Player::Boolean(std::string_view key, bool value)
{
    return prism::app::Boolean(host_, key, value);
}

bool Player::Number(std::string_view key, double value)
{
    return prism::app::Number(host_, key, value);
}

std::string Player::Time(std::uint32_t seconds)
{
    const auto remainder = seconds % 60;
    return std::to_string(seconds / 60) + ":" + (remainder < 10 ? "0" : "") +
           std::to_string(remainder);
}

bool Player::Progress()
{
    const auto duration =
        catalogue_.tracks.empty() ? 0 : catalogue_.tracks[track_].duration_seconds;
    return Text("playback_elapsed", Time(static_cast<std::uint32_t>(progress_ * duration))) &&
           Text("playback_duration", Time(duration)) &&
           Number("playback_progress", catalogue_.tracks.empty() ? 0 : progress_);
}

bool Player::PublishTrack()
{
    if (catalogue_.tracks.empty()) {
        return Text("track_title", "Preparing library") && Text("track_artist", "") && Progress();
    }
    const auto &track = catalogue_.tracks[track_];
    return Text("track_title", track.title) && Text("track_artist", track.artist) && Progress();
}

bool Player::PublishRows()
{
    row_count_ = 0;
    for (std::size_t index = 0; index < catalogue_.tracks.size(); ++index) {
        if (!favorites_ || catalogue_.tracks[index].favorite) {
            rows_[row_count_++] = index;
        }
    }
    for (std::size_t row = 0; row < MaxTracks; ++row) {
        const auto suffix = std::to_string(row + 1);
        const bool exists = row < row_count_;
        const Track *track = exists ? &catalogue_.tracks[rows_[row]] : nullptr;
        if (!Boolean("library_row_" + suffix, exists) ||
            !Text("library_title_" + suffix, track ? track->title : "") ||
            !Text("library_artist_" + suffix, track ? track->artist : "") ||
            !Text("library_duration_" + suffix, track ? Time(track->duration_seconds) : "")) {
            return false;
        }
    }
    return true;
}

bool Player::PublishPage()
{
    return Boolean("artwork_visible", !library_) && Boolean("library_visible", library_) &&
           Boolean("metadata_visible", !library_) &&
           Number("transport_height", library_ ? 60 : 104) && PublishRows();
}

bool Player::Error(std::string_view message)
{
    pending_ = false;
    playing_ = false;
    catalogue_valid_ = false;
    return Boolean("catalog_pending", false) && Boolean("catalog_error", true) &&
           Boolean("catalog_ready", false) && Text("playback_icon", "play") &&
           Text("catalog_status", "Library error: " + std::string(message));
}

bool Player::Load()
{
    if (pending_) {
        return true;
    }
    const auto request = nlohmann::json(CatalogueRequest{assets_root_}).dump();
    PrismWorkRequestV1 job{};
    job.struct_size = sizeof(job);
    job.task_id = CatalogueTaskId;
    job.priority = PRISM_WORK_CRITICAL_V1;
    job.reserve_bytes = 1024 * 1024;
    job.max_result_bytes = 16 * 1024;
    job.input = {reinterpret_cast<const std::uint8_t *>(request.data()), request.size()};
    job.work = PrepareCatalogue;
    const auto submitted = host_->submit_work(host_->context, &job);
    if (submitted != PRISM_WORK_ACCEPTED_V1) {
        return Error(submitted == PRISM_WORK_BUSY_V1 ? "Preparation queue is busy; retry"
                                                     : "Cannot queue catalogue preparation");
    }

    pending_ = true;
    playing_ = false;
    return Boolean("catalog_pending", true) && Boolean("catalog_error", false) &&
           Boolean("catalog_ready", false) && Text("catalog_status", "Loading library") &&
           Text("playback_icon", "play");
}

bool Player::Initialize()
{
    return PublishPage() && PublishTrack() && Load();
}

void Player::Completed(const PrismWorkCompletionV1 &completion)
{
    if (completion.task_id != CatalogueTaskId || !pending_) {
        return;
    }
    pending_ = false;
    if (completion.status != PRISM_WORK_SUCCEEDED_V1) {
        Error(completion.status == PRISM_WORK_CANCELLED_V1 ? "Preparation cancelled; retry"
                                                           : "Preparation failed; retry");
        return;
    }
    if (!completion.result.data && completion.result.size) {
        Error("Invalid catalogue result");
        return;
    }

    const std::string_view bytes(reinterpret_cast<const char *>(completion.result.data),
                                 completion.result.size);
    CatalogueResult result;
    try {
        result = DecodeCatalogueResult(bytes);
    } catch (const std::exception &) {
        Error("Invalid catalogue result");
        return;
    }
    if (!result.ok) {
        Error(result.error);
        return;
    }
    catalogue_ = std::move(result.catalogue);
    catalogue_valid_ = true;
    track_ = 0;
    progress_ = 0.42;
    if (!PublishTrack() || !PublishPage() || !Boolean("catalog_pending", false) ||
        !Boolean("catalog_error", false) || !Boolean("catalog_ready", true) ||
        !Text("catalog_status", "Library ready") || !Text("playback_icon", "play")) {
        Error("Cannot apply catalogue bindings");
        return;
    }
    if (!ready_) {
        if (!prism::app::Ready(host_)) {
            Error("Host rejected catalogue readiness");
            return;
        }
        ready_ = true;
    }
}

bool Player::Schedule()
{
    return prism::app::Tick(host_, 500000000ULL);
}

void Player::Select(std::size_t index)
{
    track_ = index;
    progress_ = 0;
    PublishTrack();
}

void Player::Action(std::string_view action)
{
    if (action == "player:retry") {
        Load();
    } else if (action == "nav:library" || action == "nav:favorites") {
        library_ = action == "nav:favorites" || !library_ || favorites_;
        favorites_ = action == "nav:favorites";
        PublishPage();
    } else if (pending_ || !catalogue_valid_) {
        return;
    } else if (action == "player:toggle") {
        playing_ = !playing_;
        Text("playback_icon", playing_ ? "pause" : "play");
        if (playing_) {
            Schedule();
        }
    } else if (action == "player:next" || action == "player:previous") {
        const auto count = catalogue_.tracks.size();
        Select((track_ + (action == "player:next" ? 1 : count - 1)) % count);
    } else if (action == "library:track:1" || action == "library:track:2" ||
               action == "library:track:3") {
        const auto row = static_cast<std::size_t>(action.back() - '1');
        if (row < row_count_) {
            Select(rows_[row]);
        }
    }
}

void Player::Tick()
{
    if (!playing_ || pending_ || !catalogue_valid_) {
        return;
    }
    // This demo advances UI progress; it does not decode or output audio.
    progress_ = progress_ + 0.02 > 1.0 ? 0 : progress_ + 0.02;
    Progress();
    Schedule();
}
} // namespace prism::music
