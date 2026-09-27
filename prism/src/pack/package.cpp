#include "prism/pack/package.hpp"
#include "prism/compiler/binary_generator.hpp"
#include "prism/compiler/lexer.hpp"
#include "prism/compiler/parser.hpp"
#include "prism/core/logging.hpp"
#include "prism/core/types.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace prism::pack {

static std::string ReadFileString(const std::string &path)
{
    std::ifstream file(path);
    if (!file) {
        return "";
    }
    std::stringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

static bool CompilePrismFile(const std::string &prism_path, const std::string &prismb_path)
{
    std::string source = ReadFileString(prism_path);
    if (source.empty()) {
        return false;
    }

    try {
        compiler::Lexer lexer(source);
        auto tokens = lexer.Tokenize();
        compiler::Parser parser(tokens);
        auto ast = parser.Parse();
        if (!ast) {
            return false;
        }

        compiler::BinaryGenerator gen;
        return gen.WriteToFile(ast, prismb_path);
    } catch (...) {
        return false;
    }
}

namespace {
struct LegacyManifest {
    std::string app_id;
    std::string name;
    std::string version;
    std::string exec;
};

void from_json(const nlohmann::json &json, LegacyManifest &manifest)
{
    // Missing or empty fields retain the legacy package defaults.
    (void)json.get_ref<const nlohmann::json::object_t &>();
    manifest.app_id = json.value("app_id", std::string{});
    manifest.name = json.value("name", std::string{});
    manifest.version = json.value("version", std::string{});
    manifest.exec = json.value("exec", std::string{});
}

struct PackageItem {
    std::string internal_name;
    fs::path host_path;
    std::vector<uint8_t> data;
};

void AppendFile(std::vector<PackageItem> &items, const std::string &name, const fs::path &path)
{
    if (!fs::exists(path)) {
        return;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }
    PackageItem item;
    item.internal_name = name;
    item.host_path = path;
    item.data = std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
    items.push_back(std::move(item));
    PRISM_LOG_DEBUG("PACK", "Added file entry: %s (%zu bytes)", name.c_str(),
                    items.back().data.size());
}
} // namespace

bool PackageManager::PackDirectory(const std::string &src_dir, const std::string &output_pkg_path,
                                   const std::string &bin_dir)
{
    fs::path dir(src_dir);
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        PRISM_LOG_ERROR("PACK", "Source directory does not exist: %s", src_dir.c_str());
        return false;
    }

    fs::path manifest_path = dir / "manifest.json";
    if (!fs::exists(manifest_path)) {
        PRISM_LOG_ERROR("PACK", "Missing manifest.json in %s", src_dir.c_str());
        return false;
    }

    LegacyManifest manifest;
    try {
        manifest =
            nlohmann::json::parse(ReadFileString(manifest_path.string())).get<LegacyManifest>();
    } catch (const nlohmann::json::exception &error) {
        PRISM_LOG_ERROR("PACK", "Invalid manifest.json: %s", error.what());
        return false;
    }
    std::string app_id = manifest.app_id;
    std::string name = manifest.name;
    std::string version = manifest.version;
    std::string exec = manifest.exec;

    if (app_id.empty()) {
        app_id = dir.filename().string();
    }
    if (name.empty()) {
        name = app_id;
    }
    if (version.empty()) {
        version = "1.0.0";
    }
    if (exec.empty()) {
        exec = app_id;
    }

    PRISM_LOG_INFO("PACK", "Packaging Prism App '%s' (%s v%s)...", name.c_str(), app_id.c_str(),
                   version.c_str());

    // 1. Ensure preview.prismb is compiled
    fs::path preview_prism = dir / "preview.prism";
    fs::path preview_prismb = dir / "preview.prismb";
    if (fs::exists(preview_prism)) {
        PRISM_LOG_INFO("PACK", "AOT compiling preview.prism -> preview.prismb...");
        if (!CompilePrismFile(preview_prism.string(), preview_prismb.string())) {
            PRISM_LOG_ERROR("PACK", "Failed to compile preview.prism");
            return false;
        }
    }

    // 2. Ensure master.prismb is compiled
    fs::path master_prism = dir / "master.prism";
    fs::path master_prismb = dir / "master.prismb";
    if (fs::exists(master_prism)) {
        PRISM_LOG_INFO("PACK", "AOT compiling master.prism -> master.prismb...");
        if (!CompilePrismFile(master_prism.string(), master_prismb.string())) {
            PRISM_LOG_ERROR("PACK", "Failed to compile master.prism");
            return false;
        }
    }

    // 3. Locate backend binary
    fs::path exec_binary;
    if (!bin_dir.empty()) {
        exec_binary = fs::path(bin_dir) / exec;
    }
    if (!fs::exists(exec_binary)) {
        exec_binary = dir / exec;
    }
    if (!fs::exists(exec_binary)) {
        // Fallback to build directory
        fs::path build_exec = fs::path("build/demos") / app_id / exec;
        if (fs::exists(build_exec)) {
            exec_binary = build_exec;
        } else {
            build_exec = fs::path("build/demos") / exec;
            if (fs::exists(build_exec)) {
                exec_binary = build_exec;
            }
        }
    }

    // 4. Gather package entries
    std::vector<PackageItem> items;
    AppendFile(items, "manifest.json", manifest_path);
    if (fs::exists(preview_prismb)) {
        AppendFile(items, "preview.prismb", preview_prismb);
    }
    if (fs::exists(master_prismb)) {
        AppendFile(items, "master.prismb", master_prismb);
    }
    if (fs::exists(exec_binary)) {
        AppendFile(items, exec, exec_binary);
    }

    // 5. Build package header and entries
    PackageHeader hdr{};
    std::memcpy(hdr.magic, "PRISMPKG", 8);
    hdr.version = 1;
    hdr.file_count = static_cast<uint32_t>(items.size());
    std::strncpy(hdr.app_id, app_id.c_str(), sizeof(hdr.app_id) - 1);
    std::strncpy(hdr.app_name, name.c_str(), sizeof(hdr.app_name) - 1);
    std::strncpy(hdr.version_str, version.c_str(), sizeof(hdr.version_str) - 1);
    std::strncpy(hdr.exec_entry, exec.c_str(), sizeof(hdr.exec_entry) - 1);

    std::vector<FileEntry> entries(items.size());
    uint64_t current_offset = sizeof(PackageHeader) + sizeof(FileEntry) * items.size();

    for (size_t i = 0; i < items.size(); ++i) {
        std::strncpy(entries[i].file_name, items[i].internal_name.c_str(),
                     sizeof(entries[i].file_name) - 1);
        entries[i].file_offset = current_offset;
        entries[i].file_size = items[i].data.size();
        entries[i].checksum = core::HashSlot(items[i].internal_name);
        current_offset += items[i].data.size();
    }

    // 6. Write package output
    std::ofstream out(output_pkg_path, std::ios::binary);
    if (!out) {
        PRISM_LOG_ERROR("PACK", "Cannot open output package path: %s", output_pkg_path.c_str());
        return false;
    }

    out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    for (const auto &ent : entries) {
        out.write(reinterpret_cast<const char *>(&ent), sizeof(ent));
    }
    for (const auto &item : items) {
        out.write(reinterpret_cast<const char *>(item.data.data()), item.data.size());
    }

    PRISM_LOG_INFO("PACK", "Successfully built bundle: %s (%zu files, %lu total bytes)",
                   output_pkg_path.c_str(), items.size(), current_offset);
    return true;
}

std::optional<PackageInfo> PackageManager::Inspect(const std::string &pkg_path)
{
    std::ifstream in(pkg_path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    PackageHeader hdr{};
    in.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    if (in.gcount() < static_cast<std::streamsize>(sizeof(hdr))) {
        return std::nullopt;
    }
    if (std::memcmp(hdr.magic, "PRISMPKG", 8) != 0) {
        PRISM_LOG_ERROR("PACK", "Invalid package magic in %s", pkg_path.c_str());
        return std::nullopt;
    }

    PackageInfo info;
    info.app_id = hdr.app_id;
    info.app_name = hdr.app_name;
    info.version = hdr.version_str;
    info.exec_entry = hdr.exec_entry;
    info.entries.resize(hdr.file_count);

    for (uint32_t i = 0; i < hdr.file_count; ++i) {
        in.read(reinterpret_cast<char *>(&info.entries[i]), sizeof(FileEntry));
    }

    in.seekg(0, std::ios::end);
    info.total_size = in.tellg();

    return info;
}

bool PackageManager::Unpack(const std::string &pkg_path, const std::string &dest_dir)
{
    auto info = Inspect(pkg_path);
    if (!info) {
        return false;
    }

    fs::create_directories(dest_dir);

    std::ifstream in(pkg_path, std::ios::binary);
    if (!in) {
        return false;
    }

    for (const auto &ent : info->entries) {
        fs::path out_file = fs::path(dest_dir) / ent.file_name;
        in.seekg(ent.file_offset);
        std::vector<char> buffer(ent.file_size);
        in.read(buffer.data(), ent.file_size);

        std::ofstream out(out_file, std::ios::binary);
        if (!out) {
            return false;
        }
        out.write(buffer.data(), buffer.size());

        // Make executable if it matches exec_entry
        if (std::string(ent.file_name) == info->exec_entry) {
            fs::permissions(out_file, fs::perms::owner_all | fs::perms::group_read |
                                          fs::perms::group_exec | fs::perms::others_read |
                                          fs::perms::others_exec);
        }
    }

    PRISM_LOG_INFO("PACK", "Extracted package '%s' into '%s'", pkg_path.c_str(), dest_dir.c_str());
    return true;
}

std::vector<uint8_t> PackageManager::ExtractFile(const std::string &pkg_path,
                                                 const std::string &internal_file_name)
{
    auto info = Inspect(pkg_path);
    if (!info) {
        return {};
    }

    for (const auto &ent : info->entries) {
        if (std::string(ent.file_name) == internal_file_name) {
            std::ifstream in(pkg_path, std::ios::binary);
            if (!in) {
                return {};
            }
            in.seekg(ent.file_offset);
            std::vector<uint8_t> buf(ent.file_size);
            in.read(reinterpret_cast<char *>(buf.data()), ent.file_size);
            return buf;
        }
    }
    return {};
}

} // namespace prism::pack
