#pragma once

#include "prism/contracts/owner_task.hpp"
#include "prism/runtime/task_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prism::runtime {

inline constexpr std::size_t kMaxFileTaskEntries = 4096;
inline constexpr std::size_t kMaxFileTaskScannedEntries = 16384;
inline constexpr std::uint64_t kMaxFileTaskOutputBytes = 4ULL * 1024 * 1024;

enum class FileEntryKind { Directory, RegularFile };
enum class FileTaskOperation { Directory, ValidateCandidate };
enum class FileTaskErrorCode {
    InvalidRequest,
    NotFound,
    NotDirectory,
    Unsupported,
    AccessDenied,
    TooLarge,
    Cancelled,
    Budget,
    Io
};

struct FileTaskError {
    FileTaskErrorCode code{FileTaskErrorCode::Io};
    int system_error{};
    std::string diagnostic;
    bool operator==(const FileTaskError &) const = default;
};

struct FileStamp {
    std::uint64_t device{}, inode{}, size{};
    std::int64_t modified_seconds{}, changed_seconds{};
    std::uint32_t modified_nanoseconds{}, changed_nanoseconds{};
    bool operator==(const FileStamp &) const = default;
};

struct DirectoryEntry {
    // IDs are stable in this snapshot only, never a filesystem permission.
    std::uint64_t id{};
    std::string name;
    FileEntryKind kind{FileEntryKind::RegularFile};
    bool operator==(const DirectoryEntry &) const = default;
};

struct DirectorySnapshot {
    std::uint64_t generation{};
    std::string canonical_directory;
    std::vector<DirectoryEntry> entries;
    std::size_t excluded_invalid_names{}, excluded_unsupported{};
    bool operator==(const DirectorySnapshot &) const = default;
};

struct FileCandidate {
    contracts::OwnerTaskKind kind{contracts::OwnerTaskKind::OpenFile};
    std::string canonical_path;
    bool target_exists{};
    std::optional<FileStamp> target_stamp;
    // The directory used for resolution; current-directory selection uses its
    // own stamp. These observations require revalidation before later writes.
    FileStamp parent_stamp;
    bool operator==(const FileCandidate &) const = default;
};

// Published through shared_ptr<const FileTaskResult>. The owning result retains
// the scheduler's memory lease; keep it alive while reading its nested values.
struct FileTaskResult final : TaskOutput {
    std::uint64_t generation{};
    FileTaskOperation operation{FileTaskOperation::Directory};
    std::optional<DirectorySnapshot> directory;
    std::optional<FileCandidate> candidate;
    std::optional<FileTaskError> error;

    std::uint64_t RetainedBytes() const noexcept override;
};

// One final-runtime Host owner coordinates a separate shared-scheduler channel.
// Workers own plain inputs and do all filesystem I/O. No UI, module callbacks or
// writes run here. Blocking filesystem calls observe cancellation at boundaries.
class FileTaskModel {
public:
    explicit FileTaskModel(std::shared_ptr<TaskScheduler> scheduler);
    ~FileTaskModel();
    FileTaskModel(const FileTaskModel &) = delete;
    FileTaskModel &operator=(const FileTaskModel &) = delete;
    FileTaskModel(FileTaskModel &&) = delete;
    FileTaskModel &operator=(FileTaskModel &&) = delete;

    // Pure text validation throws invalid_argument. A Busy shared scheduler
    // retains only the latest request for retry on its capacity notification.
    // Directory aliases are canonicalized on a worker; results are canonical.
    std::uint64_t StartDirectory(std::string absolute_directory,
                                 std::vector<std::string> extensions = {},
                                 bool show_hidden = false);
    // SelectDirectory accepts an empty name to validate the current directory.
    // Candidate symlinks and special files are rejected without following them.
    std::uint64_t StartValidateCandidate(contracts::OwnerTaskKind kind,
                                         std::string absolute_directory, std::string name,
                                         std::vector<std::string> extensions = {});

    std::uint64_t CurrentGeneration() const noexcept;
    bool Pending() const noexcept;
    int Fd() const noexcept;
    // Call on this channel's completion/capacity event. Nonblocking; never does
    // filesystem I/O. Stale generations are drained without publication.
    bool Advance();
    std::shared_ptr<const FileTaskResult> TakeResult();
    void Cancel();
    // Cancels this channel, waits only for its active work, and drops all results.
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::runtime
