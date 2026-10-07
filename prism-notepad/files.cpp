#include "files.hpp"

#include "prism/contracts/owner_task.hpp"
#include "prism/runtime/text_buffer.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace prism::notepad {
namespace {
struct File {
    int fd{-1};

    explicit File(int value = -1) : fd(value)
    {
    }

    ~File()
    {
        if (fd >= 0) {
            close(fd);
        }
    }

    File(const File &) = delete;
    File &operator=(const File &) = delete;

    File(File &&other) noexcept : fd(std::exchange(other.fd, -1))
    {
    }

    File &operator=(File &&) = delete;
};

void Check(bool success, const char *operation)
{
    if (!success) {
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
    }
}

void Cancel(const PrismWorkContextV1 *context)
{
    if (context && context->is_cancelled && context->is_cancelled(context->context)) {
        throw std::runtime_error("Operation cancelled");
    }
}

FileStamp Stamp(const struct stat &value)
{
    if (value.st_size < 0 || value.st_mtim.tv_nsec < 0 || value.st_mtim.tv_nsec >= 1000000000 ||
        value.st_ctim.tv_nsec < 0 || value.st_ctim.tv_nsec >= 1000000000) {
        throw std::runtime_error("File metadata is outside supported limits");
    }
    return {true,
            static_cast<std::uint64_t>(value.st_dev),
            static_cast<std::uint64_t>(value.st_ino),
            static_cast<std::uint64_t>(value.st_size),
            value.st_mtim.tv_sec,
            value.st_mtim.tv_nsec,
            value.st_ctim.tv_sec,
            value.st_ctim.tv_nsec};
}

bool SameDirectory(const struct stat &value, const FileStamp &stamp)
{
    return S_ISDIR(value.st_mode) && static_cast<std::uint64_t>(value.st_dev) == stamp.device &&
           static_cast<std::uint64_t>(value.st_ino) == stamp.inode;
}

struct ParentDirectory {
    std::string path, name, file_path;
    File file;
    FileStamp stamp;
};

void VerifyParent(const ParentDirectory &parent)
{
    std::error_code error;
    const auto resolved = std::filesystem::canonical(parent.path, error);
    if (error || resolved.native() != parent.path) {
        throw std::runtime_error("The destination folder changed; choose its location again");
    }

    File current(open(parent.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    Check(current.fd >= 0, "Reopen file folder");
    struct stat value{};
    Check(fstat(current.fd, &value) == 0, "Inspect file folder");
    if (!SameDirectory(value, parent.stamp)) {
        throw std::runtime_error("The destination folder was replaced; choose its location again");
    }
}

ParentDirectory OpenParent(const std::string &path, const PrismWorkContextV1 *context)
{
    if (path.empty() || !contracts::ValidOwnerFileDirectoryHint(path)) {
        throw std::runtime_error("Choose a valid absolute UTF-8 file path");
    }
    const std::filesystem::path source(path);
    auto name = source.filename().native();
    if (!contracts::ValidOwnerFileName(name)) {
        throw std::runtime_error("Choose a file name without a slash or control characters");
    }

    Cancel(context);
    std::error_code error;
    auto canonical = std::filesystem::canonical(source.parent_path(), error);
    if (error) {
        throw std::runtime_error("Resolve file folder: " + error.message());
    }
    auto directory = canonical.native();
    auto file_path = directory + (directory == "/" ? "" : "/") + name;
    if (!contracts::ValidOwnerFilePath(file_path)) {
        throw std::runtime_error("Resolved file path exceeds supported limits");
    }

    File file(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    Check(file.fd >= 0, "Open file folder");
    struct stat value{};
    Check(fstat(file.fd, &value) == 0, "Inspect file folder");
    ParentDirectory parent{std::move(directory), std::move(name), std::move(file_path),
                           std::move(file), Stamp(value)};
    VerifyParent(parent);
    Cancel(context);
    return parent;
}

FileStamp Inspect(const ParentDirectory &parent, struct stat &value)
{
    if (fstatat(parent.file.fd, parent.name.c_str(), &value, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) {
            return {};
        }
        Check(false, "Inspect file");
    }
    if (!S_ISREG(value.st_mode)) {
        throw std::runtime_error(
            "Choose a regular file; symbolic links and devices are not supported");
    }
    return Stamp(value);
}

std::string Read(const ParentDirectory &parent, FileStamp &stamp, const PrismWorkContextV1 *context)
{
    File file(openat(parent.file.fd, parent.name.c_str(),
                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    Check(file.fd >= 0, "Open file");
    struct stat metadata{};
    Check(fstat(file.fd, &metadata) == 0, "Inspect open file");
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
        metadata.st_size > static_cast<off_t>(MaxTextBytes)) {
        throw std::runtime_error("Only regular UTF-8 text files up to 48 KiB are supported");
    }
    stamp = Stamp(metadata);

    std::string text;
    char buffer[4096];
    for (;;) {
        Cancel(context);
        const auto count = read(file.fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        Check(count >= 0, "Read file");
        if (!count) {
            break;
        }
        text.append(buffer, static_cast<std::size_t>(count));
        if (text.size() > MaxTextBytes) {
            throw std::runtime_error("File exceeds 48 KiB");
        }
    }

    Check(fstat(file.fd, &metadata) == 0, "Inspect read file");
    if (stamp != Stamp(metadata) || Inspect(parent, metadata) != stamp) {
        throw std::runtime_error("File changed while opening; try again");
    }
    VerifyParent(parent);
    if (!runtime::TextBuffer::Valid(text)) {
        throw std::runtime_error("Not valid UTF-8 text (binary and legacy encodings are rejected)");
    }
    Cancel(context);
    return text;
}

std::uint64_t TemporaryNonce()
{
    std::uint64_t value{};
    std::size_t offset = 0;
    while (offset < sizeof(value)) {
        const auto count =
            getrandom(reinterpret_cast<char *>(&value) + offset, sizeof(value) - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        Check(count > 0, "Generate temporary name");
        offset += static_cast<std::size_t>(count);
    }
    return value;
}

struct Temporary {
    int directory;
    std::string name;
    File file;

    explicit Temporary(const ParentDirectory &parent) : directory(parent.file.fd)
    {
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            auto candidate = ".prism-notepad-" + std::to_string(TemporaryNonce());
            const int opened = openat(directory, candidate.c_str(),
                                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (opened >= 0) {
                name = std::move(candidate);
                file.fd = opened;
                return;
            }
            if (errno != EEXIST) {
                Check(false, "Create temporary file");
            }
        }
        throw std::runtime_error("Could not create a unique temporary file");
    }

    ~Temporary()
    {
        if (!name.empty()) {
            unlinkat(directory, name.c_str(), 0);
        }
    }

    void Unlink()
    {
        Check(unlinkat(directory, name.c_str(), 0) == 0, "Remove temporary name");
        name.clear();
    }

    Temporary(const Temporary &) = delete;
    Temporary &operator=(const Temporary &) = delete;
};

void Write(const FileJob &job, Temporary &temporary, const PrismWorkContextV1 *context)
{
    std::size_t offset = 0;
    while (offset < job.text.size()) {
        Cancel(context);
        const auto count =
            write(temporary.file.fd, job.text.data() + offset, job.text.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        Check(count > 0, "Write file");
        offset += static_cast<std::size_t>(count);
    }
    Check(fsync(temporary.file.fd) == 0, "Flush file");
}

FileStamp Save(const FileJob &job, const ParentDirectory &parent, const PrismWorkContextV1 *context)
{
    if (job.text.size() > MaxTextBytes || !runtime::TextBuffer::Valid(job.text) ||
        (!job.expected.exists && job.expected != FileStamp{}) || job.expected.nanos < 0 ||
        job.expected.nanos >= 1000000000 || job.expected.changed_nanos < 0 ||
        job.expected.changed_nanos >= 1000000000) {
        throw std::runtime_error("Text or expected file metadata is invalid");
    }
    struct stat metadata{};
    const auto before = Inspect(parent, metadata);
    const bool accept_existing = !job.expected.exists && job.overwrite_approved;
    if (before != job.expected && !(accept_existing && before.exists)) {
        throw std::runtime_error(
            "File already exists or changed on disk; choose a new path or reopen it");
    }
    if (before.exists && metadata.st_nlink > 1) {
        throw std::runtime_error("Hard-linked files require saving to a new path");
    }

    Cancel(context);
    Temporary temporary(parent);
    if (before.exists) {
        Check(fchmod(temporary.file.fd, metadata.st_mode & 0777) == 0, "Preserve permissions");
    }
    Write(job, temporary, context);

    Cancel(context);
    VerifyParent(parent);
    if (Inspect(parent, metadata) != before || (before.exists && metadata.st_nlink > 1)) {
        throw std::runtime_error("File changed before saving; original retained");
    }
    // POSIX has no compare-and-replace operation. The final metadata check and
    // rename are separate: a competing writer in that narrow interval remains
    // possible. An absent destination uses linkat, which never overwrites it.
    if (before.exists) {
        Check(renameat(parent.file.fd, temporary.name.c_str(), parent.file.fd,
                       parent.name.c_str()) == 0,
              "Replace file");
        temporary.name.clear();
    } else {
        Check(linkat(parent.file.fd, temporary.name.c_str(), parent.file.fd, parent.name.c_str(),
                     0) == 0,
              "Create destination without overwriting");
    }

    try {
        if (!temporary.name.empty()) {
            temporary.Unlink();
        }
        Check(fsync(parent.file.fd) == 0, "Flush directory");
        Check(fstat(temporary.file.fd, &metadata) == 0, "Inspect saved file");
        const auto saved = Stamp(metadata);
        if (Inspect(parent, metadata) != saved) {
            throw std::runtime_error("The saved destination changed immediately after commit");
        }
        VerifyParent(parent);
        return saved;
    } catch (const std::exception &error) {
        throw std::runtime_error(std::string(error.what()) + " (file may already be saved)");
    }
}
} // namespace

void to_json(nlohmann::json &json, const FileStamp &stamp)
{
    json = {{"exists", stamp.exists},
            {"device", stamp.device},
            {"inode", stamp.inode},
            {"size", stamp.size},
            {"seconds", stamp.seconds},
            {"nanos", stamp.nanos},
            {"changed_seconds", stamp.changed_seconds},
            {"changed_nanos", stamp.changed_nanos}};
}

void from_json(const nlohmann::json &json, FileStamp &stamp)
{
    json.at("exists").get_to(stamp.exists);
    json.at("device").get_to(stamp.device);
    json.at("inode").get_to(stamp.inode);
    json.at("size").get_to(stamp.size);
    json.at("seconds").get_to(stamp.seconds);
    json.at("nanos").get_to(stamp.nanos);
    stamp.changed_seconds = json.value("changed_seconds", std::int64_t{});
    stamp.changed_nanos = json.value("changed_nanos", std::int64_t{});
}

void to_json(nlohmann::json &json, const FileJob &job)
{
    json = {{"operation", job.operation},
            {"path", job.path},
            {"text", job.text},
            {"expected", job.expected},
            {"overwrite_approved", job.overwrite_approved}};
}

void from_json(const nlohmann::json &json, FileJob &job)
{
    json.at("operation").get_to(job.operation);
    json.at("path").get_to(job.path);
    json.at("text").get_to(job.text);
    json.at("expected").get_to(job.expected);
    job.overwrite_approved = json.value("overwrite_approved", false);
}

FileResult ProcessFile(const FileJob &job, const PrismWorkContextV1 *context)
{
    FileResult result;
    try {
        if (job.operation != "open" && job.operation != "save") {
            throw std::runtime_error("Unknown file operation");
        }
        const auto parent = OpenParent(job.path, context);
        result.path = parent.file_path;
        if (job.operation == "open") {
            result.text = Read(parent, result.stamp, context);
        } else {
            result.stamp = Save(job, parent, context);
        }
    } catch (const std::exception &error) {
        result.text.clear();
        result.stamp = {};
        result.error = error.what();
    }
    return result;
}

int32_t FileWork(const PrismWorkContextV1 *context, PrismBytesViewV1 input) noexcept
{
    if (!context || !context->set_result || !input.data || !input.size || input.size > 64 * 1024) {
        return -1;
    }
    try {
        const auto job =
            nlohmann::json::from_cbor(input.data, input.data + input.size).get<FileJob>();
        const auto result = ProcessFile(job, context);
        const auto bytes = nlohmann::json::to_cbor(nlohmann::json(result));
        return context->set_result(context->context, {bytes.data(), bytes.size()});
    } catch (...) {
        return -1;
    }
}
} // namespace prism::notepad
