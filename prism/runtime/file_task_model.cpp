#include "file_task_model_p.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace prism::runtime {

std::uint64_t FileTaskResult::RetainedBytes() const noexcept
{
    std::uint64_t bytes = sizeof(FileTaskResult);
    if (directory) {
        bytes += directory->canonical_directory.capacity() +
                 directory->entries.capacity() * sizeof(DirectoryEntry);
        for (const auto &entry : directory->entries) {
            bytes += entry.name.capacity();
        }
    }
    if (candidate) {
        bytes += candidate->canonical_path.capacity();
    }
    if (error) {
        bytes += error->diagnostic.capacity();
    }
    return bytes;
}

FileTaskModel::Impl::Impl(std::shared_ptr<TaskScheduler> value) : scheduler(std::move(value))
{
    if (!scheduler) {
        throw std::invalid_argument("File task model requires the shared runtime scheduler");
    }
    channel = scheduler->OpenChannel(2, 1, true);
}

bool FileTaskModel::Impl::SubmitPending()
{
    if (!pending || stopped) {
        return false;
    }
    const auto submitted = channel->Submit(
        {pending->generation, TaskPriority::Critical, kMaxFileTaskOutputBytes, *pending});
    if (submitted == TaskSubmitResult::Busy) {
        return false;
    }
    if (submitted != TaskSubmitResult::Accepted) {
        auto failed = std::make_shared<FileTaskResult>();
        failed->generation = generation;
        failed->operation = pending->operation;
        failed->error =
            FileTaskError{FileTaskErrorCode::Io, 0, "Filesystem task channel is unavailable"};
        result = std::move(failed);
        active = false;
    }
    pending.reset();
    return true;
}

std::uint64_t FileTaskModel::Impl::Start(FileTaskWork work)
{
    if (stopped) {
        throw std::logic_error("File task model is stopped");
    }
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("File task generation exhausted");
    }
    if (active && !pending) {
        channel->Cancel(generation);
    }

    result.reset();
    work.generation = ++generation;
    operation = work.operation;
    pending = std::move(work);
    active = true;
    SubmitPending();
    return generation;
}

void FileTaskModel::Impl::Complete(TaskCompletion completion)
{
    if (!active || completion.id != generation || pending) {
        return;
    }
    if (completion.error) {
        auto failed = std::make_shared<FileTaskResult>();
        failed->generation = generation;
        failed->operation = operation;
        switch (completion.error->code) {
        case TaskErrorCode::Cancelled:
            failed->error =
                FileTaskError{FileTaskErrorCode::Cancelled, 0, "Filesystem task cancelled"};
            break;
        case TaskErrorCode::Budget:
            failed->error = FileTaskError{FileTaskErrorCode::Budget, 0,
                                          "Filesystem task memory budget is unavailable"};
            break;
        case TaskErrorCode::Work:
            failed->error = FileTaskError{FileTaskErrorCode::Io, 0,
                                          "Filesystem worker could not complete the request"};
            break;
        }
        result = std::move(failed);
    } else {
        const auto output = std::dynamic_pointer_cast<const FileTaskResult>(completion.output);
        if (!output || output->generation != generation ||
            output->RetainedBytes() > kMaxFileTaskOutputBytes) {
            throw std::logic_error("Unexpected filesystem task completion");
        }
        result = output;
    }
    active = false;
}

FileTaskModel::FileTaskModel(std::shared_ptr<TaskScheduler> scheduler)
    : impl_(std::make_unique<Impl>(std::move(scheduler)))
{
}

FileTaskModel::~FileTaskModel()
{
    Stop();
}

std::uint64_t FileTaskModel::StartDirectory(std::string directory,
                                            std::vector<std::string> extensions, bool show_hidden)
{
    if (directory.empty() || !contracts::ValidOwnerFileDirectoryHint(directory) ||
        !contracts::ValidOwnerFileExtensions(extensions)) {
        throw std::invalid_argument("Invalid filesystem directory request");
    }

    FileTaskWork work;
    work.directory = std::move(directory);
    work.extensions = std::move(extensions);
    work.show_hidden = show_hidden;
    return impl_->Start(std::move(work));
}

std::uint64_t FileTaskModel::StartValidateCandidate(contracts::OwnerTaskKind kind,
                                                    std::string directory, std::string name,
                                                    std::vector<std::string> extensions)
{
    if ((kind != contracts::OwnerTaskKind::OpenFile && kind != contracts::OwnerTaskKind::SaveFile &&
         kind != contracts::OwnerTaskKind::SelectDirectory) ||
        directory.empty() || !contracts::ValidOwnerFileDirectoryHint(directory) ||
        (!contracts::ValidOwnerFileName(name) &&
         !(name.empty() && kind == contracts::OwnerTaskKind::SelectDirectory)) ||
        !contracts::ValidOwnerFileExtensions(extensions)) {
        throw std::invalid_argument("Invalid filesystem candidate request");
    }

    FileTaskWork work;
    work.operation = FileTaskOperation::ValidateCandidate;
    work.kind = kind;
    work.directory = std::move(directory);
    work.name = std::move(name);
    work.extensions = std::move(extensions);
    return impl_->Start(std::move(work));
}

std::uint64_t FileTaskModel::CurrentGeneration() const noexcept
{
    return impl_->generation;
}

bool FileTaskModel::Pending() const noexcept
{
    return impl_->active;
}

int FileTaskModel::Fd() const noexcept
{
    return impl_->channel->Fd();
}

bool FileTaskModel::Advance()
{
    auto &self = *impl_;
    if (self.stopped) {
        return false;
    }
    const auto previous = self.result;
    while (auto completion = self.channel->TakeCompletion()) {
        self.Complete(std::move(*completion));
    }
    const bool submitted = self.SubmitPending();
    return submitted || previous != self.result;
}

std::shared_ptr<const FileTaskResult> FileTaskModel::TakeResult()
{
    return std::exchange(impl_->result, {});
}

void FileTaskModel::Cancel()
{
    auto &self = *impl_;
    if (self.active && !self.pending) {
        self.channel->Cancel(self.generation);
    }
    self.pending.reset();
    self.result.reset();
    self.active = false;
}

void FileTaskModel::Stop()
{
    auto &self = *impl_;
    if (self.stopped) {
        return;
    }
    self.stopped = true;
    self.pending.reset();
    self.result.reset();
    self.active = false;
    self.channel->Stop();
}

} // namespace prism::runtime
