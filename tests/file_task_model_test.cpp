#include "prism/runtime/file_task_model.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace prism;
using namespace prism::runtime;
using namespace std::chrono_literals;

namespace {

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(bool memory = false)
    {
        auto name = std::string(memory ? "/dev/shm/prism-file-model-XXXXXX"
                                       : "/tmp/prism-file-model-XXXXXX");
        const auto *created = mkdtemp(name.data());
        assert(created);
        path_ = created;
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::string &Path() const
    {
        return path_;
    }

    void Directory(std::string_view name) const
    {
        assert(std::filesystem::create_directory(path_ + "/" + std::string(name)));
    }

    void File(std::string_view name, std::string_view text = "file") const
    {
        std::ofstream output(path_ + "/" + std::string(name), std::ios::binary);
        assert(output);
        output << text;
        output.close();
        assert(output);
    }

    void Link(std::string_view name, std::string_view target) const
    {
        assert(symlink(std::string(target).c_str(), (path_ + "/" + std::string(name)).c_str()) ==
               0);
    }

private:
    std::string path_;
};

std::shared_ptr<TaskScheduler> Scheduler(std::size_t workers = 2, std::size_t outstanding = 128)
{
    return std::make_shared<TaskScheduler>(SessionTaskBudget::Create({2, 32ULL * 1024 * 1024, 0}),
                                           workers, outstanding);
}

void Readable(int fd)
{
    pollfd ready{fd, POLLIN, 0};
    assert(poll(&ready, 1, 5000) == 1);
    assert(ready.revents == POLLIN);
}

std::shared_ptr<const FileTaskResult> Take(FileTaskModel &model)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        model.Advance();
        if (auto result = model.TakeResult()) {
            assert(result->generation == model.CurrentGeneration());
            assert(!model.Pending());
            assert(result->RetainedBytes() <= kMaxFileTaskOutputBytes);
            return result;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        assert(remaining.count() > 0);
        pollfd ready{model.Fd(), POLLIN, 0};
        assert(poll(&ready, 1, static_cast<int>(remaining.count())) == 1);
        assert(ready.revents == POLLIN);
    }
}

std::vector<std::string> Names(const DirectorySnapshot &snapshot)
{
    std::vector<std::string> names;
    for (std::size_t index = 0; index < snapshot.entries.size(); ++index) {
        const auto &entry = snapshot.entries[index];
        assert(entry.id == index + 1 && contracts::ValidOwnerFileName(entry.name));
        names.push_back(entry.name);
    }
    return names;
}

void Error(FileTaskModel &model, FileTaskErrorCode code, FileTaskOperation operation)
{
    const auto result = Take(model);
    assert(result->error && result->error->code == code);
    assert(result->operation == operation);
    assert(!result->error->diagnostic.empty());
    assert(!result->directory && !result->candidate);
}

void ListingFiltersAndNativeNames()
{
    TemporaryDirectory directory;
    directory.Directory("zeta-dir");
    directory.Directory("alpha-dir");
    directory.File("alpha.txt");
    directory.File("UPPER.txt");
    directory.File("note.md");
    directory.File("archive.tar.gz");
    directory.File("slash\\name.txt");
    directory.File(".hidden.txt");
    directory.File("\xff.txt");
    directory.File("line\nname");
    directory.Link("inside-link", directory.Path() + "/alpha.txt");
    directory.Link("directory-link", directory.Path() + "/alpha-dir");
    directory.Link("dangling-link", directory.Path() + "/missing");
    assert(mkfifo((directory.Path() + "/pipe").c_str(), 0600) == 0);

    FileTaskModel model(Scheduler());
    const auto generation = model.StartDirectory(directory.Path() + "/alpha-dir/..//");
    assert(generation && model.CurrentGeneration() == generation && model.Pending());
    const auto all = Take(model);
    assert(all->operation == FileTaskOperation::Directory && !all->error && !all->candidate);
    assert(all->directory && all->directory->generation == generation);
    assert(all->directory->canonical_directory == directory.Path());
    assert(all->directory->excluded_invalid_names == 2);
    assert(all->directory->excluded_unsupported == 4);
    assert((Names(*all->directory) == std::vector<std::string>{"alpha-dir", "zeta-dir", "UPPER.txt",
                                                               "alpha.txt", "archive.tar.gz",
                                                               "note.md", "slash\\name.txt"}));
    assert(all->directory->entries[0].kind == FileEntryKind::Directory);
    assert(all->directory->entries[2].kind == FileEntryKind::RegularFile);

    model.StartDirectory(directory.Path());
    const auto repeated = Take(model);
    assert(repeated->generation > generation &&
           repeated->directory->entries == all->directory->entries);
    model.StartDirectory(directory.Path(), {".txt"});
    const auto filtered = Take(model);
    assert((Names(*filtered->directory) == std::vector<std::string>{"alpha-dir", "zeta-dir",
                                                                    "UPPER.txt", "alpha.txt",
                                                                    "slash\\name.txt"}));
    model.StartDirectory(directory.Path(), {".TXT"});
    assert((Names(*Take(model)->directory) == std::vector<std::string>{"alpha-dir", "zeta-dir"}));
    model.StartDirectory(directory.Path(), {".tar.gz"});
    assert((Names(*Take(model)->directory) ==
            std::vector<std::string>{"alpha-dir", "zeta-dir", "archive.tar.gz"}));
    model.StartDirectory(directory.Path(), {".txt"}, true);
    const auto hidden = Take(model);
    assert(Names(*hidden->directory).at(2) == ".hidden.txt");
}

void CandidatesAreReadOnlyAndStamped()
{
    TemporaryDirectory directory;
    directory.Directory("folder");
    directory.File("document.txt", "original");
    directory.Link("link.txt", directory.Path() + "/document.txt");
    directory.Link("folder-link", directory.Path() + "/folder");
    assert(mkfifo((directory.Path() + "/pipe.txt").c_str(), 0600) == 0);
    FileTaskModel model(Scheduler());

    model.StartValidateCandidate(contracts::OwnerTaskKind::OpenFile, directory.Path(),
                                 "document.txt", {".txt"});
    const auto opened = Take(model);
    assert(opened->operation == FileTaskOperation::ValidateCandidate && !opened->error);
    assert(opened->candidate && opened->candidate->target_exists &&
           opened->candidate->target_stamp);
    assert(opened->candidate->canonical_path == directory.Path() + "/document.txt");
    assert(opened->candidate->target_stamp->size == 8);
    assert(opened->candidate->target_stamp->inode && opened->candidate->parent_stamp.inode);
    const auto stamp = *opened->candidate->target_stamp;
    model.StartValidateCandidate(contracts::OwnerTaskKind::SaveFile, directory.Path(),
                                 "document.txt", {".txt"});
    const auto overwrite = Take(model);
    assert(overwrite->candidate && overwrite->candidate->target_exists);
    assert(overwrite->candidate->target_stamp == stamp);
    assert(std::filesystem::file_size(overwrite->candidate->canonical_path) == 8);

    model.StartValidateCandidate(contracts::OwnerTaskKind::SaveFile, directory.Path(), "new.txt",
                                 {".txt"});
    const auto save = Take(model);
    assert(save->candidate && !save->candidate->target_exists && !save->candidate->target_stamp);
    assert(save->candidate->canonical_path == directory.Path() + "/new.txt");
    assert(!std::filesystem::exists(save->candidate->canonical_path));
    model.StartValidateCandidate(contracts::OwnerTaskKind::SelectDirectory, directory.Path(), "");
    const auto current = Take(model);
    assert(current->candidate && current->candidate->canonical_path == directory.Path());
    assert(current->candidate->target_stamp == current->candidate->parent_stamp);
    model.StartValidateCandidate(contracts::OwnerTaskKind::SelectDirectory, directory.Path(),
                                 "folder");
    assert(Take(model)->candidate->canonical_path == directory.Path() + "/folder");

    for (const auto name : {"link.txt", "pipe.txt", "folder"}) {
        model.StartValidateCandidate(contracts::OwnerTaskKind::OpenFile, directory.Path(), name);
        Error(model, FileTaskErrorCode::Unsupported, FileTaskOperation::ValidateCandidate);
    }
    model.StartValidateCandidate(contracts::OwnerTaskKind::SelectDirectory, directory.Path(),
                                 "folder-link");
    Error(model, FileTaskErrorCode::Unsupported, FileTaskOperation::ValidateCandidate);
    model.StartValidateCandidate(contracts::OwnerTaskKind::OpenFile, directory.Path(),
                                 "missing.txt");
    Error(model, FileTaskErrorCode::NotFound, FileTaskOperation::ValidateCandidate);
    model.StartValidateCandidate(contracts::OwnerTaskKind::SaveFile, directory.Path(), "bad.md",
                                 {".txt"});
    Error(model, FileTaskErrorCode::Unsupported, FileTaskOperation::ValidateCandidate);

    directory.File("document.txt", "different size");
    model.StartValidateCandidate(contracts::OwnerTaskKind::OpenFile, directory.Path(),
                                 "document.txt");
    assert(Take(model)->candidate->target_stamp != stamp);
}

void InvalidInputsAndFilesystemErrors()
{
    TemporaryDirectory directory;
    directory.File("file.txt");
    FileTaskModel model(Scheduler());
    for (const auto &path : {std::string{}, std::string("relative"), std::string("/bad\npath")}) {
        bool rejected = false;
        try {
            model.StartDirectory(path);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected && !model.Pending() && model.CurrentGeneration() == 0);
    }
    for (const auto &name :
         {std::string{}, std::string("../escape"), std::string(".."), std::string(256, 'x')}) {
        bool rejected = false;
        try {
            model.StartValidateCandidate(contracts::OwnerTaskKind::OpenFile, directory.Path(),
                                         name);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected && !model.Pending());
    }
    bool rejected = false;
    try {
        model.StartDirectory(directory.Path(), {"txt"});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
    model.StartDirectory(directory.Path() + "/missing");
    Error(model, FileTaskErrorCode::NotFound, FileTaskOperation::Directory);
    model.StartDirectory(directory.Path() + "/file.txt");
    Error(model, FileTaskErrorCode::NotDirectory, FileTaskOperation::Directory);
}

struct SmallOutput final : TaskOutput {
    std::uint64_t RetainedBytes() const noexcept override
    {
        return sizeof(SmallOutput);
    }
};

class Barrier {
public:
    std::shared_ptr<const TaskOutput> Work(std::stop_token stop)
    {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, stop, [this] { return released_; });
        return std::make_shared<const SmallOutput>();
    }

    void Await()
    {
        std::unique_lock lock(mutex_);
        assert(changed_.wait_for(lock, 5s, [this] { return entered_; }));
    }

    void Release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    bool entered_{}, released_{};
};

TaskCompletion TakeChannel(TaskChannel &channel)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        if (auto result = channel.TakeCompletion()) {
            return std::move(*result);
        }
        assert(std::chrono::steady_clock::now() < deadline);
        Readable(channel.Fd());
    }
}

void BusyRetryAndRapidNavigation()
{
    TemporaryDirectory first, second, third;
    first.File("first.txt");
    second.File("second.txt");
    third.File("third.txt");
    for (const auto capacity : {std::size_t(1), std::size_t(2)}) {
        auto scheduler = Scheduler(1, capacity);
        auto blocker = scheduler->OpenChannel();
        Barrier barrier;
        assert(blocker->Submit(
                   {1, TaskPriority::Critical, 64, std::bind_front(&Barrier::Work, &barrier)}) ==
               TaskSubmitResult::Accepted);
        barrier.Await();
        FileTaskModel model(scheduler);
        const auto a = model.StartDirectory(first.Path());
        const auto b = model.StartDirectory(second.Path());
        const auto c = model.StartDirectory(third.Path());
        assert(a < b && b < c && model.CurrentGeneration() == c && model.Pending());
        model.Advance();
        assert(!model.TakeResult());
        barrier.Release();
        assert(TakeChannel(*blocker).id == 1);
        const auto result = Take(model);
        assert(result->generation == c && result->directory && !result->error);
        assert((Names(*result->directory) == std::vector<std::string>{"third.txt"}));
        model.Advance();
        assert(!model.TakeResult());
    }
}

void CompletedStaleCancellationAndStop()
{
    TemporaryDirectory first, second;
    first.File("first.txt");
    second.File("second.txt");
    auto scheduler = Scheduler(1, 2);
    FileTaskModel model(scheduler);
    const auto old = model.StartDirectory(first.Path());
    Readable(model.Fd());
    const auto next = model.StartDirectory(second.Path());
    const auto result = Take(model);
    assert(next > old && result->generation == next && result->directory);
    assert((Names(*result->directory) == std::vector<std::string>{"second.txt"}));

    auto blocker = scheduler->OpenChannel();
    Barrier barrier;
    assert(blocker->Submit(
               {1, TaskPriority::Critical, 64, std::bind_front(&Barrier::Work, &barrier)}) ==
           TaskSubmitResult::Accepted);
    barrier.Await();
    model.StartDirectory(first.Path());
    assert(model.Pending());
    model.Cancel();
    assert(!model.Pending() && !model.TakeResult());
    model.Stop(); // Its queued work stops without stopping the other active channel.
    model.Stop();
    assert(!model.Advance() && !model.TakeResult());
    bool rejected = false;
    try {
        model.StartDirectory(first.Path());
    } catch (const std::logic_error &) {
        rejected = true;
    }
    assert(rejected);
    barrier.Release();
    assert(TakeChannel(*blocker).id == 1);
}

void ExactEntryLimitAndBudgetFailure()
{
    TemporaryDirectory directory(true);
    for (std::size_t index = 0; index < kMaxFileTaskEntries; ++index) {
        directory.File(std::to_string(index) + ".txt", "");
    }
    FileTaskModel model(Scheduler());
    model.StartDirectory(directory.Path());
    const auto complete = Take(model);
    assert(complete->directory && !complete->error);
    assert(complete->directory->entries.size() == kMaxFileTaskEntries);
    directory.File("one-too-many.txt", "");
    model.StartDirectory(directory.Path());
    Error(model, FileTaskErrorCode::TooLarge, FileTaskOperation::Directory);

    // A shared reservation failure is distinct from the provider's TooLarge;
    // no partial directory vector is exposed in either case.
    auto scheduler = std::make_shared<TaskScheduler>(SessionTaskBudget::Create({1, 1024, 0}), 1);
    FileTaskModel budgeted(scheduler);
    budgeted.StartValidateCandidate(contracts::OwnerTaskKind::SaveFile, directory.Path(),
                                    "save.txt");
    Error(budgeted, FileTaskErrorCode::Budget, FileTaskOperation::ValidateCandidate);

    TemporaryDirectory hidden(true);
    for (std::size_t index = 0; index < kMaxFileTaskScannedEntries; ++index) {
        hidden.File(".hidden-" + std::to_string(index), "");
    }
    model.StartDirectory(hidden.Path());
    const auto scanned = Take(model);
    assert(scanned->directory && scanned->directory->entries.empty() && !scanned->error);
    hidden.File(".one-too-many", "");
    model.StartDirectory(hidden.Path());
    Error(model, FileTaskErrorCode::TooLarge, FileTaskOperation::Directory);
}

} // namespace

int main()
{
    ListingFiltersAndNativeNames();
    CandidatesAreReadOnlyAndStamped();
    InvalidInputsAndFilesystemErrors();
    BusyRetryAndRapidNavigation();
    CompletedStaleCancellationAndStop();
    ExactEntryLimitAndBudgetFailure();
}
