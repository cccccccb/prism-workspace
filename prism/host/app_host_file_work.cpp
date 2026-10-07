#include "app_host_p.hpp"

namespace prism::sdk {
namespace {
std::string FileErrorText(runtime::FileTaskErrorCode code)
{
    using Code = runtime::FileTaskErrorCode;
    switch (code) {
    case Code::AccessDenied:
        return "Cannot access this location. Choose another folder.";
    case Code::NotFound:
        return "The item no longer exists. Refresh or choose another.";
    case Code::NotDirectory:
        return "Choose an existing folder.";
    case Code::Unsupported:
        return "Choose a regular file or folder, without a symbolic link.";
    case Code::TooLarge:
        return "This location has too many items. Choose a smaller folder.";
    case Code::Budget:
        return "File preparation is unavailable. Try again.";
    case Code::InvalidRequest:
        return "Check the file name and allowed type.";
    case Code::Cancelled:
        return "File preparation was cancelled. Try again.";
    case Code::Io:
        return "Cannot read this location. Choose another folder or retry.";
    }
    return "File preparation failed.";
}

bool SameOverwriteTarget(const runtime::FileCandidate &before, const runtime::FileCandidate &after)
{
    return before.canonical_path == after.canonical_path && before.target_exists &&
           after.target_exists && before.target_stamp &&
           before.target_stamp == after.target_stamp &&
           before.parent_stamp.device == after.parent_stamp.device &&
           before.parent_stamp.inode == after.parent_stamp.inode;
}
} // namespace

void AppHost::Impl::AdvanceFileTask()
{
    if (!owner_task || !owner_task->file || !owner_task->identity || !file_model ||
        owner_task->cancel_requested) {
        return;
    }
    const auto identity = *owner_task->identity;
    const auto active = frontend->ActiveOwnerTask();
    if (!active || active->identity != identity) {
        return;
    }
    if (file_model->Pending() && active->phase == runtime::TaskPhase::Ready) {
        frontend->SetOwnerTaskWorking(identity);
    }

    file_model->Advance();
    if (const auto result = file_model->TakeResult()) {
        auto &task = *owner_task;
        auto &file = *task.file;
        if (result->generation != file.generation) {
            return;
        }
        if (result->error) {
            file.phase = OwnerFileState::Phase::Browsing;
            file.validated.reset();
            file.overwrite.reset();
            file.status = FileErrorText(result->error->code);
            PublishFilePanel();
            return;
        }
        if (result->directory) {
            file.listing = result;
            file.directory = result->directory->canonical_directory;
            file.items.clear();
            for (std::size_t index = 0; index < result->directory->entries.size(); ++index) {
                if (task.request.kind != contracts::OwnerTaskKind::SelectDirectory ||
                    result->directory->entries[index].kind == runtime::FileEntryKind::Directory) {
                    file.items.push_back(index);
                }
            }
            file.page = 0;
            file.selected.reset();
            file.phase = OwnerFileState::Phase::Browsing;
            file.status = file.items.empty() ? "This folder is empty." : std::string{};
            if (result->directory->excluded_invalid_names ||
                result->directory->excluded_unsupported) {
                file.status = "Some items cannot be selected by this file service.";
            }
            PublishFilePanel();
            return;
        }
        if (!result->candidate) {
            frontend->FailOwnerTask(identity, {runtime::TaskFailureCode::OperationFailed,
                                               "File preparation returned no candidate"});
            return;
        }

        if (file.phase == OwnerFileState::Phase::Revalidating) {
            if (!file.overwrite || !SameOverwriteTarget(*file.overwrite, *result->candidate)) {
                file.phase = OwnerFileState::Phase::Browsing;
                file.overwrite.reset();
                file.status = "The destination changed. Review it before saving.";
                PublishFilePanel();
                return;
            }
            file.overwrite_approved = true;
        } else if (task.request.kind == contracts::OwnerTaskKind::SaveFile &&
                   result->candidate->target_exists) {
            file.overwrite = result->candidate;
            file.phase = OwnerFileState::Phase::Overwrite;
            file.status = "A file with this name already exists.";
            PublishFilePanel();
            return;
        } else {
            file.overwrite_approved = false;
        }
        file.validated = result->candidate;
    }

    if (!owner_task || owner_task->identity != identity || !owner_task->file ||
        !owner_task->file->validated || owner_task->cancel_requested) {
        return;
    }
    const auto current = frontend->ActiveOwnerTask();
    if (!current || current->identity != identity ||
        current->phase == runtime::TaskPhase::Preparing) {
        return;
    }

    owner_task->file_path = owner_task->file->validated->canonical_path;
    owner_task->overwrite_approved = owner_task->file->overwrite_approved;
    frontend->CompleteOwnerTask(identity);
}
} // namespace prism::sdk
