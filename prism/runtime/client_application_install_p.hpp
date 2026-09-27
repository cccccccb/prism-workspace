#pragma once

#include "prism/runtime/scene_construction.hpp"
#include "prism/sdk/client_application.hpp"
#include <map>
#include <set>
#include <vector>

namespace prism::sdk {
struct StagedImage {
    std::string uri;
    contracts::ResourceId id{};
    bool registered{};
    bool uploaded{};
};

struct StagedUnit {
    std::string region;
    runtime::PreparedComponent prepared;
};

struct StagedUiInstall {
    runtime::UiLoadId load;
    bool regions{};
    std::vector<StagedUnit> units;
    std::vector<StagedImage> images;
    std::map<std::string, contracts::ResourceId, std::less<>> image_ids;
    std::set<std::uint64_t> owned_images;
    std::set<std::uint64_t> result_images;
    std::vector<runtime::RegionUpdate> updates;
    std::unique_ptr<runtime::SceneConstruction> construction;
    std::unique_ptr<runtime::Scene> scene;
    std::optional<contracts::ThemeSnapshot> constructed_theme;
    std::uint64_t transaction_revision{};
    std::vector<contracts::ResourceId> candidate_images;
    std::size_t candidate_images_ready{};
    std::size_t requested{};
    std::size_t registered{};

    contracts::ResourceId Resolve(std::string_view uri) const
    {
        const auto found = image_ids.find(uri);
        return found == image_ids.end() ? contracts::ResourceId{} : found->second;
    }

    void ResetCandidate()
    {
        scene.reset();
        construction.reset();
        candidate_images.clear();
        candidate_images_ready = 0;
        result_images.clear();
        if (!regions) {
            updates.clear();
        }
    }
};
} // namespace prism::sdk
