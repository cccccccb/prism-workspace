#include "../prism-notepad/files.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace prism::notepad;

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory()
    {
        std::string name = "/tmp/prism-notepad-work-XXXXXX";
        const auto *created = mkdtemp(name.data());
        assert(created);
        path = created;
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void Write(const std::filesystem::path &path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    assert(output);
}

std::string Read(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

bool HasTemporary(const std::filesystem::path &directory, std::size_t size)
{
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().native().starts_with(".prism-notepad-") &&
            entry.file_size() == size) {
            return true;
        }
    }
    return false;
}

void NoTemporaries(const std::filesystem::path &directory)
{
    for (const auto &entry : std::filesystem::recursive_directory_iterator(directory)) {
        assert(!entry.path().filename().native().starts_with(".prism-notepad-"));
    }
}

void RewriteWithOriginalMtime(const std::filesystem::path &path, std::string_view text)
{
    struct stat before{};
    assert(stat(path.c_str(), &before) == 0);
    // Give the real filesystem's ctime clock a distinct tick. This is not a
    // performance test; restoring mtime below must not erase the detected edit.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Write(path, text);
    const timespec times[]{before.st_atim, before.st_mtim};
    assert(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
}

struct Mutation {
    enum class Kind {
        CancelImmediately,
        CancelAfterWrite,
        ChangeBeforeCommit,
        ReplaceBeforeCommit,
        CollisionBeforeCommit,
        ReplaceParent,
        SymlinkParent,
        AddHardLink,
        ReplaceDuringRead,
        ChangeDuringRead
    };
    Kind kind;
    std::filesystem::path directory;
    std::string name;
    std::size_t written_size{};
    unsigned calls{};
    bool performed{};
    bool callback_failed{};

    bool Ready() const
    {
        if (kind == Kind::CancelImmediately) {
            return true;
        }
        if (kind == Kind::ReplaceDuringRead || kind == Kind::ChangeDuringRead) {
            return calls >= 3;
        }
        return HasTemporary(directory, written_size);
    }

    void Perform()
    {
        const auto target = directory / name;
        switch (kind) {
        case Kind::CancelImmediately:
        case Kind::CancelAfterWrite:
            break;
        case Kind::ChangeBeforeCommit:
        case Kind::ChangeDuringRead:
            RewriteWithOriginalMtime(target, "external");
            break;
        case Kind::ReplaceBeforeCommit:
        case Kind::ReplaceDuringRead: {
            const auto replacement = directory / "replacement";
            Write(replacement, "external");
            std::filesystem::rename(replacement, target);
            break;
        }
        case Kind::CollisionBeforeCommit:
            Write(target, "collision");
            break;
        case Kind::ReplaceParent:
        case Kind::SymlinkParent: {
            const auto moved = std::filesystem::path(directory.native() + "-moved");
            std::filesystem::rename(directory, moved);
            if (kind == Kind::ReplaceParent) {
                std::filesystem::create_directory(directory);
                Write(target, "replacement folder");
            } else {
                std::filesystem::create_directory_symlink(moved, directory);
            }
            break;
        }
        case Kind::AddHardLink:
            std::filesystem::create_hard_link(target, directory / "another-link");
            break;
        }
        performed = true;
    }

    static int32_t Cancelled(void *context) noexcept
    {
        auto &self = *static_cast<Mutation *>(context);
        try {
            ++self.calls;
            if (!self.performed && self.Ready()) {
                self.Perform();
            }
            return self.performed &&
                   (self.kind == Kind::CancelImmediately || self.kind == Kind::CancelAfterWrite);
        } catch (...) {
            self.callback_failed = true;
            return 1;
        }
    }

    PrismWorkContextV1 Context()
    {
        return {sizeof(PrismWorkContextV1), this, Cancelled, nullptr, -1};
    }
};

void BasicReadAndSave()
{
    TemporaryDirectory temp;
    const auto path = (temp.path / "note.txt").native();
    const auto saved = ProcessFile({"save", path, "hello\n你好\r\n\t", {}});
    assert(saved.error.empty() && saved.stamp.exists && saved.path == path);
    const auto opened = ProcessFile({"open", path, {}, {}});
    assert(opened.error.empty() && opened.text == "hello\n你好\r\n\t");
    assert(opened.stamp == saved.stamp);

    assert(chmod(path.c_str(), 0640) == 0);
    const auto loaded = ProcessFile({"open", path, {}, {}});
    assert(loaded.error.empty());
    const auto updated = ProcessFile({"save", path, "updated", loaded.stamp});
    assert(updated.error.empty() && Read(path) == "updated");
    struct stat metadata{};
    assert(stat(path.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0640);
    assert(ProcessFile({"open", path, {}, {}}).stamp == updated.stamp);
    assert(!ProcessFile({"save", path, "stale", loaded.stamp}).error.empty());
    assert(Read(path) == "updated");
    NoTemporaries(temp.path);
}

void ApprovalAndFullStamp()
{
    TemporaryDirectory temp;
    const auto path = (temp.path / "note.txt").native();
    Write(path, "original");
    const auto original = ProcessFile({"open", path, {}, {}});
    assert(original.error.empty());
    assert(!ProcessFile({"save", path, "denied", {}}).error.empty());
    assert(Read(path) == "original");
    const auto approved = ProcessFile({"save", path, "approved", {}, true});
    assert(approved.error.empty() && Read(path) == "approved");
    assert(ProcessFile({"open", path, {}, {}}).stamp == approved.stamp);

    RewriteWithOriginalMtime(path, "external");
    const auto changed = ProcessFile({"open", path, {}, {}});
    assert(changed.error.empty() && changed.stamp.device == approved.stamp.device &&
           changed.stamp.inode == approved.stamp.inode &&
           changed.stamp.size == approved.stamp.size);
    assert(changed.stamp.seconds == approved.stamp.seconds &&
           changed.stamp.nanos == approved.stamp.nanos);
    assert(changed.stamp.changed_seconds != approved.stamp.changed_seconds ||
           changed.stamp.changed_nanos != approved.stamp.changed_nanos);
    assert(!ProcessFile({"save", path, "must not overwrite", approved.stamp, true}).error.empty());
    assert(Read(path) == "external");

    auto wrong_mtime = changed.stamp;
    ++wrong_mtime.seconds;
    assert(!ProcessFile({"save", path, "must not overwrite", wrong_mtime, true}).error.empty());
    const auto fresh = ProcessFile({"save", path, "fresh approval", {}, true});
    assert(fresh.error.empty() && Read(path) == "fresh approval");
    const auto created = ProcessFile({"save", (temp.path / "new.txt").native(), "new", {}, true});
    assert(created.error.empty());
    NoTemporaries(temp.path);
}

void TypesBoundsAndCanonicalPath()
{
    TemporaryDirectory temp;
    const auto path = temp.path / "note.txt";
    Write(path, "ordinary");
    std::filesystem::create_symlink(path, temp.path / "link");
    assert(!ProcessFile({"open", (temp.path / "link").native(), {}, {}}).error.empty());
    assert(!ProcessFile({"save", (temp.path / "link").native(), "x", {}, true}).error.empty());
    assert(Read(path) == "ordinary");
    assert(mkfifo((temp.path / "fifo").c_str(), 0600) == 0);
    assert(!ProcessFile({"open", (temp.path / "fifo").native(), {}, {}}).error.empty());
    assert(!ProcessFile({"save", (temp.path / "fifo").native(), "x", {}, true}).error.empty());
    assert(!ProcessFile({"save", temp.path.native(), "x", {}, true}).error.empty());

    std::filesystem::create_hard_link(path, temp.path / "hard");
    const auto hard = ProcessFile({"open", (temp.path / "hard").native(), {}, {}});
    assert(hard.error.empty());
    assert(!ProcessFile({"save", (temp.path / "hard").native(), "x", hard.stamp}).error.empty());
    assert(!ProcessFile({"save", (temp.path / "hard").native(), "x", {}, true}).error.empty());
    assert(Read(path) == "ordinary");

    const auto maximum = ProcessFile(
        {"save", (temp.path / "maximum.txt").native(), std::string(MaxTextBytes, 'x'), {}});
    assert(maximum.error.empty());
    assert(ProcessFile({"open", maximum.path, {}, {}}).text.size() == MaxTextBytes);
    assert(!ProcessFile({"save", maximum.path, std::string(MaxTextBytes + 1, 'x'), maximum.stamp})
                .error.empty());
    Write(temp.path / "large", std::string(MaxTextBytes + 1, 'x'));
    assert(!ProcessFile({"open", (temp.path / "large").native(), {}, {}}).error.empty());
    Write(temp.path / "binary", std::string("a\0b", 3));
    assert(!ProcessFile({"open", (temp.path / "binary").native(), {}, {}}).error.empty());
    assert(!ProcessFile({"save", (temp.path / "invalid").native(), "\xc0\xaf", {}}).error.empty());

    const auto real = temp.path / "real";
    std::filesystem::create_directory(real);
    std::filesystem::create_directory_symlink(real, temp.path / "alias");
    const auto alias = ProcessFile(
        {"save", (temp.path / "alias" / "name\\literal.txt").native(), "canonical", {}});
    assert(alias.error.empty() && alias.path == (real / "name\\literal.txt").native());
    assert(!ProcessFile({"save", "relative.txt", "x", {}}).error.empty());
    assert(!ProcessFile({"save", temp.path.native() + "/bad\nname", "x", {}}).error.empty());
    assert(!ProcessFile({"save", (temp.path / "missing" / "note.txt").native(), "x", {}})
                .error.empty());
    NoTemporaries(temp.path);
}

void CommitRechecksAndCleanup()
{
    for (const auto kind : {Mutation::Kind::ChangeBeforeCommit, Mutation::Kind::ReplaceBeforeCommit,
                            Mutation::Kind::CollisionBeforeCommit, Mutation::Kind::ReplaceParent,
                            Mutation::Kind::SymlinkParent, Mutation::Kind::AddHardLink,
                            Mutation::Kind::CancelAfterWrite}) {
        TemporaryDirectory temp;
        const auto directory = temp.path / "folder";
        std::filesystem::create_directory(directory);
        const auto path = directory / "note.txt";
        const bool absent = kind == Mutation::Kind::CollisionBeforeCommit;
        if (!absent) {
            Write(path, "original");
        }
        const auto text = std::string("new text");
        Mutation mutation{kind, directory, "note.txt", text.size()};
        auto context = mutation.Context();
        const auto result = ProcessFile({"save", path.native(), text, {}, !absent}, &context);
        assert(mutation.performed && !mutation.callback_failed && !result.error.empty());
        assert(!result.stamp.exists);
        switch (kind) {
        case Mutation::Kind::ReplaceParent:
            assert(Read(path) == "replacement folder");
            assert(Read(std::filesystem::path(directory.native() + "-moved") / "note.txt") ==
                   "original");
            break;
        case Mutation::Kind::SymlinkParent:
        case Mutation::Kind::AddHardLink:
        case Mutation::Kind::CancelAfterWrite:
            assert(Read(path) == "original");
            break;
        case Mutation::Kind::CollisionBeforeCommit:
            assert(Read(path) == "collision");
            break;
        default:
            assert(Read(path) == "external");
            break;
        }
        NoTemporaries(temp.path);
    }

    TemporaryDirectory temp;
    Mutation mutation{Mutation::Kind::CancelImmediately, temp.path, "new.txt", 1};
    auto context = mutation.Context();
    assert(
        !ProcessFile({"save", (temp.path / "new.txt").native(), "x", {}}, &context).error.empty());
    assert(mutation.performed && !std::filesystem::exists(temp.path / "new.txt"));
    NoTemporaries(temp.path);
}

void OpenRechecksPathAndMetadata()
{
    for (const auto kind : {Mutation::Kind::ReplaceDuringRead, Mutation::Kind::ChangeDuringRead}) {
        TemporaryDirectory temp;
        Write(temp.path / "note.txt", "original");
        Mutation mutation{kind, temp.path, "note.txt", 0};
        auto context = mutation.Context();
        const auto result =
            ProcessFile({"open", (temp.path / "note.txt").native(), {}, {}}, &context);
        assert(mutation.performed && !mutation.callback_failed);
        assert(!result.error.empty() && result.text.empty() && !result.stamp.exists);
        assert(Read(temp.path / "note.txt") == "external");
    }
}

struct WorkResult {
    std::vector<std::uint8_t> bytes;
    unsigned deliveries{};

    static int32_t Set(void *context, PrismBytesViewV1 bytes)
    {
        auto &self = *static_cast<WorkResult *>(context);
        self.bytes.assign(bytes.data, bytes.data + bytes.size);
        ++self.deliveries;
        return 0;
    }
};

void TypedSerializationAndWorkerBoundary()
{
    const FileStamp stamp{true, 11, 22, 33, 44, 55, 66, 77};
    const FileJob job{"save", "/tmp/example.txt", "text", stamp, true};
    const auto decoded = nlohmann::json(job).get<FileJob>();
    assert(decoded.operation == job.operation && decoded.path == job.path &&
           decoded.text == job.text && decoded.expected == stamp && decoded.overwrite_approved);
    auto legacy = nlohmann::json(FileJob{"save", "/tmp/example.txt", "text", {}});
    legacy.erase("overwrite_approved");
    legacy["expected"].erase("changed_seconds");
    legacy["expected"].erase("changed_nanos");
    const auto old = legacy.get<FileJob>();
    assert(!old.overwrite_approved && old.expected == FileStamp{});

    TemporaryDirectory temp;
    const FileJob create{"save", (temp.path / "note.txt").native(), "worker", {}};
    const auto input = nlohmann::json::to_cbor(nlohmann::json(create));
    WorkResult result;
    PrismWorkContextV1 context{sizeof(context), &result, nullptr, WorkResult::Set, -1};
    assert(FileWork(&context, {input.data(), input.size()}) == 0);
    assert(result.deliveries == 1);
    const auto output = nlohmann::json::from_cbor(result.bytes).get<FileResult>();
    assert(output.error.empty() && output.path == create.path && Read(create.path) == "worker");
    assert(FileWork(nullptr, {input.data(), input.size()}) == -1);
    const std::vector<std::uint8_t> too_large(65537);
    assert(FileWork(&context, {too_large.data(), too_large.size()}) == -1);
    const std::uint8_t malformed[]{0xff};
    assert(FileWork(&context, {malformed, sizeof(malformed)}) == -1);
    assert(result.deliveries == 1);
}
} // namespace

int main()
{
    BasicReadAndSave();
    ApprovalAndFullStamp();
    TypesBoundsAndCanonicalPath();
    CommitRechecksAndCleanup();
    OpenRechecksPathAndMetadata();
    TypedSerializationAndWorkerBoundary();
    std::cout << "notepad_file_work_test: passed\n";
}
