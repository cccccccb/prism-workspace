#include "file_task_model_p.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace prism::runtime {
namespace {

class FileFailure {
public:
    explicit FileFailure(FileTaskError value) : error(std::move(value))
    {
    }

    FileTaskError error;
};

[[noreturn]] void Fail(FileTaskErrorCode code, const char *diagnostic, int system_error = 0)
{
    throw FileFailure({code, system_error, diagnostic});
}

void CheckStop(std::stop_token stop)
{
    if (stop.stop_requested()) {
        Fail(FileTaskErrorCode::Cancelled, "Filesystem task cancelled");
    }
}

[[noreturn]] void FailSystem(const char *diagnostic, int error)
{
    auto code = FileTaskErrorCode::Io;
    if (error == ENOENT) {
        code = FileTaskErrorCode::NotFound;
    } else if (error == ENOTDIR) {
        code = FileTaskErrorCode::NotDirectory;
    } else if (error == EACCES || error == EPERM) {
        code = FileTaskErrorCode::AccessDenied;
    } else if (error == ELOOP) {
        code = FileTaskErrorCode::Unsupported;
    }
    Fail(code, diagnostic, error);
}

FileStamp Stamp(const struct stat &info)
{
    if (info.st_size < 0 || info.st_mtim.tv_nsec < 0 || info.st_mtim.tv_nsec >= 1000000000 ||
        info.st_ctim.tv_nsec < 0 || info.st_ctim.tv_nsec >= 1000000000) {
        Fail(FileTaskErrorCode::Unsupported, "Filesystem metadata is outside supported limits");
    }
    return {static_cast<std::uint64_t>(info.st_dev),
            static_cast<std::uint64_t>(info.st_ino),
            static_cast<std::uint64_t>(info.st_size),
            info.st_mtim.tv_sec,
            info.st_ctim.tv_sec,
            static_cast<std::uint32_t>(info.st_mtim.tv_nsec),
            static_cast<std::uint32_t>(info.st_ctim.tv_nsec)};
}

struct DirectoryCloser {
    void operator()(DIR *directory) const noexcept
    {
        if (directory) {
            closedir(directory);
        }
    }
};

struct OpenedDirectory {
    std::string path;
    std::unique_ptr<DIR, DirectoryCloser> handle;
    FileStamp stamp;
};

OpenedDirectory OpenDirectory(const std::string &path, std::stop_token stop)
{
    CheckStop(stop);
    std::error_code error;
    auto canonical = std::filesystem::canonical(path, error);
    CheckStop(stop);
    if (error) {
        FailSystem("Cannot resolve directory", error.value());
    }
    auto name = canonical.native();
    if (!contracts::ValidOwnerFilePath(name)) {
        Fail(FileTaskErrorCode::Unsupported, "Resolved directory cannot be represented safely");
    }

    const int fd = open(name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        FailSystem("Cannot open directory", errno);
    }
    struct stat info{};
    if (fstat(fd, &info) != 0) {
        const int failure = errno;
        close(fd);
        FailSystem("Cannot inspect directory", failure);
    }
    if (!S_ISDIR(info.st_mode)) {
        close(fd);
        Fail(FileTaskErrorCode::NotDirectory, "The requested path is not a directory");
    }
    DIR *const handle = fdopendir(fd);
    if (!handle) {
        const int failure = errno;
        close(fd);
        FailSystem("Cannot enumerate directory", failure);
    }
    OpenedDirectory result{std::move(name), std::unique_ptr<DIR, DirectoryCloser>(handle),
                           Stamp(info)};
    CheckStop(stop);
    return result;
}

std::optional<FileEntryKind> EntryKind(const dirent &entry, int fd)
{
    if (entry.d_type == DT_DIR) {
        return FileEntryKind::Directory;
    }
    if (entry.d_type == DT_REG) {
        return FileEntryKind::RegularFile;
    }
    if (entry.d_type != DT_UNKNOWN) {
        return {};
    }

    struct stat info{};
    if (fstatat(fd, entry.d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return {}; // The directory can change between readdir and inspection.
        }
        FailSystem("Cannot inspect directory entry", errno);
    }
    if (S_ISDIR(info.st_mode)) {
        return FileEntryKind::Directory;
    }
    if (S_ISREG(info.st_mode)) {
        return FileEntryKind::RegularFile;
    }
    return {};
}

struct EntryOrder {
    bool operator()(const DirectoryEntry &left, const DirectoryEntry &right) const
    {
        if (left.kind != right.kind) {
            return left.kind == FileEntryKind::Directory;
        }
        return left.name < right.name;
    }
};

void Enumerate(const FileTaskWork &work, FileTaskResult &output, std::stop_token stop)
{
    auto source = OpenDirectory(work.directory, stop);
    output.directory.emplace();
    auto &directory = *output.directory;
    directory.generation = work.generation;
    directory.canonical_directory = source.path;
    directory.entries.reserve(64);
    std::size_t scanned = 0;
    std::uint64_t name_bytes = 0;
    for (;;) {
        CheckStop(stop);
        errno = 0;
        const auto *entry = readdir(source.handle.get());
        if (!entry) {
            if (errno) {
                FailSystem("Cannot read directory entries", errno);
            }
            break;
        }
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        if (++scanned > kMaxFileTaskScannedEntries) {
            Fail(FileTaskErrorCode::TooLarge, "Directory exceeds the 16384 scanned entry limit");
        }
        if (!contracts::ValidOwnerFileName(name)) {
            ++directory.excluded_invalid_names;
            continue;
        }
        if (!work.show_hidden && name.front() == '.') {
            continue;
        }
        const auto kind = EntryKind(*entry, dirfd(source.handle.get()));
        if (!kind) {
            ++directory.excluded_unsupported;
            continue;
        }
        if (*kind == FileEntryKind::RegularFile &&
            !contracts::OwnerFileExtensionMatches(name, work.extensions)) {
            continue;
        }
        if (directory.entries.size() == kMaxFileTaskEntries) {
            Fail(FileTaskErrorCode::TooLarge, "Directory exceeds the 4096 entry limit");
        }

        directory.entries.push_back({0, std::string(name), *kind});
        name_bytes += directory.entries.back().name.capacity();
        const auto retained = sizeof(FileTaskResult) + directory.canonical_directory.capacity() +
                              directory.entries.capacity() * sizeof(DirectoryEntry) + name_bytes;
        if (retained > kMaxFileTaskOutputBytes) {
            Fail(FileTaskErrorCode::TooLarge, "Directory exceeds the 4 MiB output limit");
        }
    }

    CheckStop(stop);
    std::sort(directory.entries.begin(), directory.entries.end(), EntryOrder{});
    CheckStop(stop);
    for (std::size_t index = 0; index < directory.entries.size(); ++index) {
        directory.entries[index].id = index + 1;
    }
}

void ValidateCandidate(const FileTaskWork &work, FileTaskResult &output, std::stop_token stop)
{
    auto directory = OpenDirectory(work.directory, stop);
    FileCandidate candidate;
    candidate.kind = work.kind;
    candidate.parent_stamp = directory.stamp;
    if (work.kind == contracts::OwnerTaskKind::SelectDirectory && work.name.empty()) {
        candidate.canonical_path = directory.path;
        candidate.target_exists = true;
        candidate.target_stamp = directory.stamp;
        output.candidate = std::move(candidate);
        return;
    }
    if (work.kind != contracts::OwnerTaskKind::SelectDirectory &&
        !contracts::OwnerFileExtensionMatches(work.name, work.extensions)) {
        Fail(FileTaskErrorCode::Unsupported, "The selected file does not match the requested type");
    }

    candidate.canonical_path = directory.path + (directory.path == "/" ? "" : "/") + work.name;
    if (!contracts::ValidOwnerFilePath(candidate.canonical_path)) {
        Fail(FileTaskErrorCode::Unsupported, "Selected path exceeds supported limits");
    }
    CheckStop(stop);
    struct stat info{};
    const bool exists =
        fstatat(dirfd(directory.handle.get()), work.name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists) {
        if (errno != ENOENT || work.kind != contracts::OwnerTaskKind::SaveFile) {
            FailSystem("Cannot inspect the selected path", errno);
        }
    } else {
        const bool supported = work.kind == contracts::OwnerTaskKind::SelectDirectory
                                   ? S_ISDIR(info.st_mode)
                                   : S_ISREG(info.st_mode);
        if (!supported) {
            Fail(FileTaskErrorCode::Unsupported,
                 "Selection must be a real directory or regular file, without symbolic links");
        }
        candidate.target_stamp = Stamp(info);
    }

    CheckStop(stop);
    candidate.target_exists = exists;
    output.candidate = std::move(candidate);
}

} // namespace

std::shared_ptr<const TaskOutput> FileTaskWork::operator()(std::stop_token stop) const
{
    auto output = std::make_shared<FileTaskResult>();
    output->generation = generation;
    output->operation = operation;
    try {
        if (operation == FileTaskOperation::Directory) {
            Enumerate(*this, *output, stop);
        } else {
            ValidateCandidate(*this, *output, stop);
        }
        CheckStop(stop);
        if (output->RetainedBytes() > kMaxFileTaskOutputBytes) {
            Fail(FileTaskErrorCode::TooLarge, "Filesystem output exceeds the 4 MiB limit");
        }
    } catch (const FileFailure &failure) {
        output->directory.reset();
        output->candidate.reset();
        output->error = failure.error;
    } catch (const std::exception &) {
        output->directory.reset();
        output->candidate.reset();
        output->error = FileTaskError{FileTaskErrorCode::Io, 0,
                                      "Filesystem worker could not prepare the result"};
    }
    return output;
}

} // namespace prism::runtime
