#pragma once
#include "prism/contracts/launch.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace prism::launch {
struct AppManifest {
    std::string app_id;
    std::string name;
    std::string version;
    std::string ui;
    std::optional<std::string> preview;
    std::string module;
    std::string assets;
    int width{640};
    int height{400};
};

struct AppPackage {
    AppManifest manifest;
    std::filesystem::path root, ui, module, assets;
    std::optional<std::filesystem::path> preview;
};

bool ValidAppId(std::string_view id);
// Strict schema v1: unknown/duplicate fields, incompatible ABI, invalid paths
// and wrong types fail. No shell role or arbitrary exec command is accepted.
AppManifest ParseManifest(std::string_view json);
// Directory-package baseline. Resolves existing resources beneath root.
// Package files must remain immutable for the lifetime of a launched instance.
AppPackage LoadRegisteredPackage(const std::filesystem::path &apps_root, std::string_view app_id);
AppPackage LoadPackage(const std::filesystem::path &root);
} // namespace prism::launch
