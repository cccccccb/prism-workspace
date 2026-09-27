#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace prism::pack {

#pragma pack(push, 1)

struct PackageHeader {
    char magic[8];    // "PRISMPKG"
    uint32_t version; // 1
    uint32_t file_count;
    char app_id[64];
    char app_name[64];
    char version_str[32];
    char exec_entry[64];
};

struct FileEntry {
    char file_name[64];
    uint64_t file_offset;
    uint64_t file_size;
    uint32_t checksum;
};

#pragma pack(pop)

struct PackageInfo {
    std::string app_id;
    std::string app_name;
    std::string version;
    std::string exec_entry;
    std::vector<FileEntry> entries;
    uint64_t total_size{0};
};

/**
 * @brief Application Packaging and Distribution Manager (.prismpkg)
 *        Bundles manifest, AOT compiled .prismb binary ASTs, and backend executable
 *        into an optimized zero-copy mmap package format.
 */
class PackageManager {
public:
    static bool PackDirectory(const std::string &src_dir, const std::string &output_pkg_path,
                              const std::string &bin_dir = "");
    static std::optional<PackageInfo> Inspect(const std::string &pkg_path);
    static bool Unpack(const std::string &pkg_path, const std::string &dest_dir);
    static std::vector<uint8_t> ExtractFile(const std::string &pkg_path,
                                            const std::string &internal_file_name);
};

} // namespace prism::pack
