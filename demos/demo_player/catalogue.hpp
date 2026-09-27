#pragma once

#include "prism/contracts/app_module.h"
#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace prism::music {
inline constexpr std::size_t MaxCatalogueBytes = 64 * 1024;
inline constexpr std::size_t MaxTracks = 3;
inline constexpr std::uint64_t CatalogueTaskId = 1;

struct Track {
    std::string id;
    std::string title;
    std::string artist;
    std::uint32_t duration_seconds{};
    bool favorite{};
};

struct Catalogue {
    std::uint32_t version{1};
    std::vector<Track> tracks;
};

struct CatalogueRequest {
    std::string assets_root;
};

struct CatalogueResult {
    bool ok{};
    Catalogue catalogue;
    std::string error;
};

void to_json(nlohmann::json &, const Track &);
void from_json(const nlohmann::json &, Track &);
void to_json(nlohmann::json &, const Catalogue &);
void from_json(const nlohmann::json &, Catalogue &);
void to_json(nlohmann::json &, const CatalogueRequest &);
void from_json(const nlohmann::json &, CatalogueRequest &);
void to_json(nlohmann::json &, const CatalogueResult &);
void from_json(const nlohmann::json &, CatalogueResult &);
CatalogueResult DecodeCatalogueResult(std::string_view);
// Pure worker entry. It receives copied request bytes and cancellation/result
// operations only; it never receives Player, Host API, Scene or GPU objects.
std::int32_t PrepareCatalogue(const PrismWorkContextV1 *, PrismBytesViewV1) noexcept;
} // namespace prism::music
