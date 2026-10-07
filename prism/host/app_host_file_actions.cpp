#include "app_host_p.hpp"

#include <algorithm>
#include <filesystem>

namespace prism::sdk {
std::optional<runtime::TaskIdentity>
AppHost::Impl::BeginFileTask(const contracts::OwnerTaskRequest &request)
{
    if (!file_model) {
        file_model = std::make_unique<runtime::FileTaskModel>(scheduler);
    }
    auto &file = owner_task->file.emplace();
    file.directory = request.file->initial_directory.empty() ? FileHomeDirectory()
                                                             : request.file->initial_directory;
    file.loading_directory = file.directory;
    file.filename = request.file->suggested_name;
    if (request.kind == contracts::OwnerTaskKind::SaveFile && file.filename.empty()) {
        file.filename = "Untitled";
        if (!request.file->extensions.empty()) {
            file.filename += request.file->extensions.front();
        }
    }

    const auto view = FilePanelView();
    const auto identity = frontend->BeginOwnerFileTask(request, view);
    if (!identity || !owner_task || owner_task->request.request_id != request.request_id) {
        return identity;
    }
    auto &current = *owner_task->file;
    current.generation = file_model->StartDirectory(current.directory, request.file->extensions,
                                                    request.file->show_hidden);
    return identity;
}

void AppHost::Impl::NavigateFileTask(std::string directory)
{
    auto &task = *owner_task;
    auto &file = *task.file;
    file.generation = file_model->StartDirectory(directory, task.request.file->extensions,
                                                 task.request.file->show_hidden);
    file.loading_directory = std::move(directory);
    file.phase = OwnerFileState::Phase::Listing;
    file.status.clear();
    file.validated.reset();
    file.overwrite.reset();
    PublishFilePanel();
}

void AppHost::Impl::ValidateFileSelection(bool overwrite)
{
    auto &task = *owner_task;
    auto &file = *task.file;
    std::string name;
    if (task.request.kind == contracts::OwnerTaskKind::SaveFile) {
        name = file.filename;
    } else if (task.request.kind == contracts::OwnerTaskKind::OpenFile) {
        if (!file.selected || !file.listing || !file.listing->directory) {
            return;
        }
        name = file.listing->directory->entries[*file.selected].name;
    }

    file.generation = file_model->StartValidateCandidate(
        task.request.kind, file.directory, std::move(name), task.request.file->extensions);
    file.phase =
        overwrite ? OwnerFileState::Phase::Revalidating : OwnerFileState::Phase::Validating;
    file.status.clear();
    file.validated.reset();
    PublishFilePanel();
}

bool AppHost::Impl::HandleFileTaskText(std::string_view action, std::string_view text)
{
    if (action != runtime::kOwnerFileNameAction) {
        return false;
    }
    if (!owner_task || !owner_task->file || !owner_task->identity || owner_task->cancel_requested ||
        owner_task->request.kind != contracts::OwnerTaskKind::SaveFile ||
        owner_task->file->phase != OwnerFileState::Phase::Browsing) {
        return true;
    }
    const auto active = frontend->ActiveOwnerTask();
    if (!active || active->identity != *owner_task->identity ||
        active->phase != runtime::TaskPhase::Ready) {
        return true;
    }

    auto size = std::min(text.size(), contracts::kMaxOwnerFileTaskPathBytes);
    if (size < text.size()) {
        while (size && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80) {
            --size;
        }
    }
    owner_task->file->filename.assign(text.substr(0, size));
    auto &file = *owner_task->file;
    if (text.size() > contracts::kMaxOwnerFileTaskNameBytes) {
        file.status = "File names must be at most 255 bytes.";
    } else if (!file.filename.empty() && !contracts::ValidOwnerFileName(file.filename)) {
        file.status = "Use a file name without a slash or control characters.";
    } else if (!file.filename.empty() && !contracts::OwnerFileExtensionMatches(
                                             file.filename, owner_task->request.file->extensions)) {
        file.status = "Use one of the allowed file extensions.";
    } else {
        file.status.clear();
    }
    PublishFilePanel();
    return true;
}

bool AppHost::Impl::HandleFileTaskAction(std::string_view action)
{
    if (!owner_task || !owner_task->file || !owner_task->identity || owner_task->cancel_requested) {
        return true;
    }
    const auto active = frontend->ActiveOwnerTask();
    if (!active || active->identity != *owner_task->identity ||
        active->phase != runtime::TaskPhase::Ready) {
        return true;
    }
    auto &file = *owner_task->file;
    if (file.phase == OwnerFileState::Phase::Overwrite) {
        if (action == runtime::kOwnerFileReplaceAction) {
            ValidateFileSelection(true);
        } else if (action == runtime::kOwnerFileBackAction) {
            file.phase = OwnerFileState::Phase::Browsing;
            file.overwrite.reset();
            file.status.clear();
            PublishFilePanel();
        }
        return true;
    }
    if (file.phase != OwnerFileState::Phase::Browsing) {
        return true;
    }

    if (action == runtime::kOwnerFileHomeAction) {
        NavigateFileTask(FileHomeDirectory());
    } else if (action == runtime::kOwnerFileUpAction) {
        const auto parent = std::filesystem::path(file.directory).parent_path().string();
        if (!parent.empty() && parent != file.directory) {
            NavigateFileTask(parent);
        }
    } else if (action == runtime::kOwnerFilePreviousAction ||
               action == runtime::kOwnerFileNextAction) {
        const auto pages = std::max<std::size_t>(1, (file.items.size() + 7) / 8);
        if (action == runtime::kOwnerFilePreviousAction && file.page) {
            --file.page;
        } else if (action == runtime::kOwnerFileNextAction && file.page + 1 < pages) {
            ++file.page;
        } else {
            return true;
        }
        PublishFilePanel();
    } else if (action == runtime::kOwnerFileSubmitAction) {
        if (FilePanelView().submit_enabled) {
            ValidateFileSelection(false);
        }
    } else {
        const auto found = std::find(runtime::kOwnerFileRowActions.begin(),
                                     runtime::kOwnerFileRowActions.end(), action);
        if (found == runtime::kOwnerFileRowActions.end() || !file.listing ||
            !file.listing->directory) {
            return true;
        }
        const auto slot = static_cast<std::size_t>(found - runtime::kOwnerFileRowActions.begin());
        const auto item = file.page * runtime::kOwnerFileRowActions.size() + slot;
        if (item >= file.items.size()) {
            return true;
        }
        const auto index = file.items[item];
        const auto &entry = file.listing->directory->entries[index];
        if (entry.kind == runtime::FileEntryKind::Directory) {
            NavigateFileTask((std::filesystem::path(file.directory) / entry.name).string());
        } else {
            file.selected = index;
            if (owner_task->request.kind == contracts::OwnerTaskKind::SaveFile) {
                file.filename = entry.name;
            }
            file.status.clear();
            PublishFilePanel();
        }
    }
    return true;
}
} // namespace prism::sdk
