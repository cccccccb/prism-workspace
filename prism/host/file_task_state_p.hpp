#pragma once

#include "prism/runtime/file_task_model.hpp"
#include "prism/runtime/owner_file_panel.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prism::sdk {
struct OwnerFileState {
    enum class Phase { Listing, Browsing, Validating, Overwrite, Revalidating };
    Phase phase{Phase::Listing};
    std::uint64_t generation{};
    std::shared_ptr<const runtime::FileTaskResult> listing;
    std::vector<std::size_t> items;
    std::string directory;
    std::string loading_directory;
    std::string filename;
    std::string status;
    std::size_t page{};
    std::optional<std::size_t> selected;
    std::optional<runtime::FileCandidate> overwrite;
    std::optional<runtime::FileCandidate> validated;
    bool overwrite_approved{};
};
} // namespace prism::sdk
