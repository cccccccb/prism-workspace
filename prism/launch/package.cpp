#include "prism/launch/package.hpp"
#include "prism/launch/error.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

namespace prism::launch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kManifestLimit = 65536;
void Require(bool condition, const char* reason) {
    if (!condition) throw LaunchFailure(contracts::LaunchError::InvalidPackage, reason);
}
void Keys(const Json& object, std::initializer_list<std::string_view> allowed) {
    Require(object.is_object(), "Manifest object expected");
    for (const auto& [key, value] : object.items()) {
        (void)value;
        Require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), "Unknown manifest field");
    }
}
std::string Text(const Json& object, const char* key) {
    const auto& value = object.at(key);
    Require(value.is_string(), "Manifest string expected");
    auto text = value.get<std::string>();
    Require(!text.empty() && text.size() <= 4096 && text.find('\0') == std::string::npos,
            "Invalid manifest string");
    return text;
}
bool Relative(const std::string& value) {
    std::filesystem::path path(value);
    if (path.empty() || path.is_absolute() || value.find('\\') != std::string::npos) return false;
    for (const auto& part : path) if (part == "..") return false;
    return true;
}
int Dimension(const Json& value) {
    Require(value.is_number_integer(), "Window dimensions must be integers");
    Require(value >= 1 && value <= 4096, "Window dimension outside 1..4096");
    return value.get<int>();
}
std::filesystem::path Resolve(const std::filesystem::path& root, const std::string& relative, bool directory) {
    const auto resolved = std::filesystem::canonical(root / relative);
    auto a = root.begin(), b = resolved.begin();
    for (; a != root.end() && b != resolved.end() && *a == *b; ++a, ++b) {}
    Require(a == root.end(), "Package resource escapes its root");
    Require(directory ? std::filesystem::is_directory(resolved) : std::filesystem::is_regular_file(resolved),
            "Package resource has wrong file type");
    return resolved;
}
} // namespace

bool ValidAppId(std::string_view id) {
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (id.empty() || id.size() > 128 || !letter(id.front())) return false;
    return std::all_of(id.begin(), id.end(), [&](char c) {
        return letter(c) || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

static AppManifest ParseManifestChecked(std::string_view text) {
    Require(!text.empty() && text.size() <= kManifestLimit, "Manifest size outside limit");
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        Require(depth <= 16, "Manifest nesting outside limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            if (!keys.back().insert(value.get<std::string>()).second) duplicate = true;
        } else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    const auto json = Json::parse(text, callback);
    Require(!duplicate, "Duplicate manifest field");
    Keys(json, {"format_version", "runtime_abi", "app_id", "name", "version", "ui", "preview", "module", "assets", "window"});
    Require(json.at("format_version").is_number_unsigned() &&
            json.at("format_version") == contracts::kAppPackageVersion, "Unsupported package format");
    Require(json.at("runtime_abi").is_number_unsigned(), "Runtime ABI must be an integer");
    if (json.at("runtime_abi") != contracts::kRuntimeAbiVersion)
        throw LaunchFailure(contracts::LaunchError::UnsupportedAbi, "Unsupported runtime ABI");
    AppManifest manifest;
    manifest.app_id = Text(json, "app_id");
    Require(ValidAppId(manifest.app_id), "Invalid application ID");
    manifest.name = Text(json, "name");
    manifest.version = Text(json, "version");
    manifest.ui = Text(json, "ui");
    manifest.module = Text(json, "module");
    manifest.assets = Text(json, "assets");
    if (json.contains("preview")) manifest.preview = Text(json, "preview");
    Require(Relative(manifest.ui) && Relative(manifest.module) && Relative(manifest.assets) &&
            (!manifest.preview || Relative(*manifest.preview)), "Package paths must be relative without traversal");
    if (json.contains("window")) {
        const auto& window = json.at("window");
        Keys(window, {"width", "height"});
        manifest.width = Dimension(window.at("width"));
        manifest.height = Dimension(window.at("height"));
    }
    return manifest;
}

AppManifest ParseManifest(std::string_view text) {
    try { return ParseManifestChecked(text); }
    catch (const Json::exception& error) {
        throw LaunchFailure(contracts::LaunchError::InvalidPackage, error.what());
    }
}

static AppPackage LoadPackageChecked(const std::filesystem::path& directory) {
    AppPackage package;
    package.root = std::filesystem::canonical(directory);
    Require(std::filesystem::is_directory(package.root), "Package root must be a directory");
    const auto manifest_file = Resolve(package.root, "manifest.json", false);
    std::ifstream input(manifest_file, std::ios::binary);
    if (!input) throw LaunchFailure(contracts::LaunchError::InvalidPackage, "Cannot read package manifest");
    std::string text(kManifestLimit + 1, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    Require(!input.bad(), "Cannot read package manifest");
    text.resize(static_cast<std::size_t>(input.gcount()));
    package.manifest = ParseManifest(text);
    package.ui = Resolve(package.root, package.manifest.ui, false);
    package.module = Resolve(package.root, package.manifest.module, false);
    package.assets = Resolve(package.root, package.manifest.assets, true);
    if (package.manifest.preview) package.preview = Resolve(package.root, *package.manifest.preview, false);
    return package;
}
AppPackage LoadPackage(const std::filesystem::path& directory) {
    try { return LoadPackageChecked(directory); }
    catch (const std::filesystem::filesystem_error& error) {
        throw LaunchFailure(contracts::LaunchError::InvalidPackage, error.what());
    }
}
} // namespace prism::launch
