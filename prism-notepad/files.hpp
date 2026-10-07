#pragma once
#include "prism/contracts/app_module.h"
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace prism::notepad {
constexpr std::size_t MaxTextBytes = 48 * 1024;

struct FileStamp {
    bool exists{};
    std::uint64_t device{}, inode{}, size{};
    std::int64_t seconds{}, nanos{};
    std::int64_t changed_seconds{}, changed_nanos{};
    bool operator==(const FileStamp &) const = default;
};

struct FileJob {
    std::string operation, path, text;
    FileStamp expected;
    // Loaded documents require their exact stamp. With no loaded stamp, this
    // flag permits capturing a fresh existing target, then rechecking it before
    // commit. Provider approval is intent, not a frozen filesystem version.
    bool overwrite_approved{};
};

struct FileResult {
    std::string path, text, error;
    FileStamp stamp;
};

void to_json(nlohmann::json &, const FileStamp &);
void from_json(const nlohmann::json &, FileStamp &);
void to_json(nlohmann::json &, const FileJob &);
void from_json(const nlohmann::json &, FileJob &);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileResult, path, text, error, stamp)
FileResult ProcessFile(const FileJob &, const PrismWorkContextV1 * = nullptr);
int32_t FileWork(const PrismWorkContextV1 *, PrismBytesViewV1) noexcept;
} // namespace prism::notepad
