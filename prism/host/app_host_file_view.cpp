#include "app_host_p.hpp"

#include <algorithm>
#include <cstdlib>

namespace prism::sdk {
std::string AppHost::Impl::FileHomeDirectory() const
{
    const auto *home = std::getenv("HOME");
    return home && contracts::ValidOwnerFileDirectoryHint(home) && *home ? home : "/";
}

runtime::OwnerFilePanelView AppHost::Impl::FilePanelView() const
{
    const auto &task = *owner_task;
    const auto &file = *task.file;
    runtime::OwnerFilePanelView view;
    view.title = task.request.title;
    view.directory =
        file.phase == OwnerFileState::Phase::Listing ? file.loading_directory : file.directory;
    view.selected_caption = view.directory;
    view.filename = file.filename;
    view.status = file.status;
    if (view.status.empty()) {
        switch (file.phase) {
        case OwnerFileState::Phase::Listing:
            view.status = "Loading folder...";
            break;
        case OwnerFileState::Phase::Validating:
            view.status = "Checking selection...";
            break;
        case OwnerFileState::Phase::Revalidating:
            view.status = "Checking destination...";
            break;
        case OwnerFileState::Phase::Browsing:
        case OwnerFileState::Phase::Overwrite:
            break;
        }
    }
    view.show_filename = task.request.kind == contracts::OwnerTaskKind::SaveFile;
    view.overwrite = file.phase == OwnerFileState::Phase::Overwrite ||
                     file.phase == OwnerFileState::Phase::Revalidating;
    view.loading = file.phase == OwnerFileState::Phase::Listing ||
                   file.phase == OwnerFileState::Phase::Validating ||
                   file.phase == OwnerFileState::Phase::Revalidating;
    view.nav_enabled = file.phase == OwnerFileState::Phase::Browsing;
    view.submit_enabled = file.phase == OwnerFileState::Phase::Overwrite;

    const auto pages =
        std::max<std::size_t>(1, (file.items.size() + view.rows.size() - 1) / view.rows.size());
    view.page_caption = std::to_string(file.page + 1) + " / " + std::to_string(pages);
    if (!view.loading && file.listing && file.listing->directory) {
        const auto &entries = file.listing->directory->entries;
        const auto start = file.page * view.rows.size();
        for (std::size_t row = 0; row < view.rows.size() && start + row < file.items.size();
             ++row) {
            const auto index = file.items[start + row];
            const auto &entry = entries[index];
            view.rows[row] = {entry.name, entry.kind == runtime::FileEntryKind::Directory,
                              file.selected == index};
        }
        if (file.phase == OwnerFileState::Phase::Browsing) {
            switch (task.request.kind) {
            case contracts::OwnerTaskKind::SaveFile:
                view.submit_enabled = contracts::ValidOwnerFileName(file.filename) &&
                                      contracts::OwnerFileExtensionMatches(
                                          file.filename, task.request.file->extensions);
                break;
            case contracts::OwnerTaskKind::SelectDirectory:
                view.submit_enabled = true;
                break;
            case contracts::OwnerTaskKind::OpenFile:
                view.submit_enabled = file.selected && entries[*file.selected].kind ==
                                                           runtime::FileEntryKind::RegularFile;
                break;
            case contracts::OwnerTaskKind::Confirmation:
                break;
            }
        }

        std::string_view name;
        if (task.request.kind == contracts::OwnerTaskKind::SaveFile &&
            contracts::ValidOwnerFileName(file.filename)) {
            name = file.filename;
        } else if (task.request.kind == contracts::OwnerTaskKind::OpenFile && file.selected &&
                   *file.selected < entries.size()) {
            name = entries[*file.selected].name;
        }
        if (!name.empty()) {
            auto path = file.directory + (file.directory == "/" ? "" : "/") + std::string(name);
            if (contracts::ValidOwnerFilePath(path)) {
                view.selected_caption = std::move(path);
            } else {
                view.status = "The selected path exceeds the supported length.";
                view.submit_enabled = false;
            }
        }
    }
    if (view.overwrite && file.overwrite) {
        view.selected_caption = file.overwrite->canonical_path;
    }
    return view;
}

bool AppHost::Impl::PublishFilePanel()
{
    if (!owner_task || !owner_task->file || !owner_task->identity || failed || closed) {
        return false;
    }
    const auto identity = *owner_task->identity;
    const auto view = FilePanelView();
    if (frontend->UpdateOwnerFileTask(identity, view)) {
        return true;
    }
    if (owner_task && owner_task->identity == identity) {
        frontend->FailOwnerTask(identity, {runtime::TaskFailureCode::PreparationFailed,
                                           "File task presentation is unavailable"});
    }
    return false;
}
} // namespace prism::sdk
