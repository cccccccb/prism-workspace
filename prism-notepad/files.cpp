#include "files.hpp"
#include "prism/runtime/text_buffer.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace prism::notepad {
namespace {
struct File {
    int fd{-1};

    explicit File(int value) : fd(value)
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
};

struct Temporary {
    std::string path;

    ~Temporary()
    {
        if (!path.empty()) {
            unlink(path.c_str());
        }
    }
};

void Check(bool success, const char *operation)
{
    if (!success) {
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
    }
}

void Cancel(const PrismWorkContextV1 *context)
{
    if (context && context->is_cancelled(context->context)) {
        throw std::runtime_error("Operation cancelled");
    }
}

FileStamp Stamp(const struct stat &value)
{
    return {true,
            value.st_dev,
            value.st_ino,
            static_cast<std::uint64_t>(value.st_size),
            value.st_mtim.tv_sec,
            value.st_mtim.tv_nsec};
}

FileStamp Inspect(const std::string &path, struct stat &value)
{
    if (lstat(path.c_str(), &value) < 0) {
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

std::string Read(const std::string &path, FileStamp &stamp, const PrismWorkContextV1 *context)
{
    File file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
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
    if (stamp != Stamp(metadata)) {
        throw std::runtime_error("File changed while opening; try again");
    }
    if (!runtime::TextBuffer::Valid(text)) {
        throw std::runtime_error("Not valid UTF-8 text (binary and legacy encodings are rejected)");
    }
    return text;
}

FileStamp Save(const FileJob &job, const std::string &path, const PrismWorkContextV1 *context)
{
    if (!runtime::TextBuffer::Valid(job.text)) {
        throw std::runtime_error("Text is invalid or exceeds 48 KiB");
    }
    struct stat metadata{};
    const auto before = Inspect(path, metadata);
    if (before != job.expected) {
        throw std::runtime_error(
            "File already exists or changed on disk; choose a new path or reopen it");
    }
    if (before.exists && metadata.st_nlink > 1) {
        throw std::runtime_error("Hard-linked files require saving to a new path");
    }
    const auto parent = std::filesystem::path(path).parent_path();
    Temporary temporary{(parent / ".prism-notepad-XXXXXX").string()};
    File file(mkstemp(temporary.path.data()));
    Check(file.fd >= 0, "Create temporary file");
    if (before.exists) {
        Check(fchmod(file.fd, metadata.st_mode & 0777) == 0, "Preserve permissions");
    }
    std::size_t offset = 0;
    while (offset < job.text.size()) {
        Cancel(context);
        const auto count = write(file.fd, job.text.data() + offset, job.text.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        Check(count > 0, "Write file");
        offset += static_cast<std::size_t>(count);
    }
    Check(fsync(file.fd) == 0, "Flush file");
    Cancel(context);
    if (Inspect(path, metadata) != before) {
        throw std::runtime_error("File changed before saving; original retained");
    }
    if (before.exists) {
        Check(rename(temporary.path.c_str(), path.c_str()) == 0, "Replace file");
    } else {
        Check(link(temporary.path.c_str(), path.c_str()) == 0,
              "Create destination without overwriting");
    }
    File directory(open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Check(directory.fd >= 0 && fsync(directory.fd) == 0,
          "Flush directory (file may already be saved)");
    return Inspect(path, metadata);
}
} // namespace

FileResult ProcessFile(const FileJob &job, const PrismWorkContextV1 *context)
{
    FileResult result;
    try {
        if (job.path.empty() || job.path.size() > 4096 ||
            job.path.find('\0') != std::string::npos ||
            !std::filesystem::path(job.path).is_absolute()) {
            throw std::runtime_error(
                "Enter an absolute file path, for example /home/you/notes.txt");
        }
        result.path = std::filesystem::path(job.path).lexically_normal().string();
        Cancel(context);
        if (job.operation == "open") {
            result.text = Read(result.path, result.stamp, context);
        } else if (job.operation == "save") {
            result.stamp = Save(job, result.path, context);
        } else {
            throw std::runtime_error("Unknown file operation");
        }
    } catch (const std::exception &error) {
        result.error = error.what();
    }
    return result;
}

int32_t FileWork(const PrismWorkContextV1 *context, PrismBytesViewV1 input) noexcept
{
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
