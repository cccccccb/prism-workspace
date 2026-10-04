#pragma once
#include "prism/contracts/app_module.h"
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
    bool operator==(const FileStamp &) const = default;
};

struct FileJob {
    std::string operation, path, text;
    FileStamp expected;
};

struct FileResult {
    std::string path, text, error;
    FileStamp stamp;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileStamp, exists, device, inode, size, seconds, nanos)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileJob, operation, path, text, expected)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileResult, path, text, error, stamp)
FileResult ProcessFile(const FileJob &, const PrismWorkContextV1 * = nullptr);
int32_t FileWork(const PrismWorkContextV1 *, PrismBytesViewV1) noexcept;
} // namespace prism::notepad
