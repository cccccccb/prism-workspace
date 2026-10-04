#include "notepad.hpp"
#include <algorithm>
#include <filesystem>

namespace prism::notepad {
Notepad::Notepad(const PrismHostApiV1 *host) : host_(host)
{
    New();
    message_ = "Ready";
    Publish();
}

void Notepad::String(std::string_view key, std::string_view text)
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_STRING_V1;
    value.as.string = {text.data(), text.size()};
    host_->set_binding(host_->context, {key.data(), key.size()}, value);
}

void Notepad::Number(std::string_view key, double number)
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_NUMBER_V1;
    value.as.number = number;
    host_->set_binding(host_->context, {key.data(), key.size()}, value);
}

void Notepad::Boolean(std::string_view key, bool state)
{
    PrismValueV1 value{};
    value.kind = PRISM_VALUE_BOOL_V1;
    value.as.boolean = state;
    host_->set_binding(host_->context, {key.data(), key.size()}, value);
}

void Notepad::Select(std::size_t index)
{
    if (!documents_[index].used) {
        return;
    }
    active_ = index;
    std::size_t rank = 0;
    for (std::size_t i = 0; i < index; ++i) {
        rank += documents_[i].used;
    }
    tab_page_ = rank / 2;
    path_ = documents_[index].path;
    confirm_ = false;
    list_ = false;
    path_panel_ = false;
}

void Notepad::New()
{
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (!documents_[i].used) {
            documents_[i] = {};
            documents_[i].used = true;
            documents_[i].title = "Untitled " + std::to_string(++untitled_);
            Select(i);
            message_ = "New document";
            return;
        }
    }
    message_ = "Eight documents are open; close one before creating another";
}

void Notepad::Close(bool discard)
{
    if (busy_) {
        message_ = "Wait for the current file operation before closing a document";
        return;
    }
    if (!discard) {
        closing_ = active_;
    }
    if (documents_[closing_].Dirty() && !discard) {
        confirm_ = true;
        return;
    }
    Remove(closing_);
}

void Notepad::Remove(std::size_t index)
{
    documents_[index] = {};
    // Reset the stable editor's undo history before reusing the slot.
    String("text_" + std::to_string(index), " ");
    String("text_" + std::to_string(index), "");
    confirm_ = false;
    close_after_save_ = false;
    if (documents_[active_].used) {
        Select(active_);
        return;
    }
    for (std::size_t step = 1; step < documents_.size(); ++step) {
        const auto candidate = (index + documents_.size() - step) % documents_.size();
        if (documents_[candidate].used) {
            Select(candidate);
            return;
        }
    }
    New();
}

void Notepad::ShowPath(bool save)
{
    path_save_ = save;
    path_panel_ = true;
    list_ = false;
    path_ = save ? documents_[active_].path : "";
    message_ = save ? "Choose an absolute file path" : "Open a UTF-8 text file";
}

void Notepad::Start(bool save)
{
    if (busy_) {
        message_ = "A file operation is already in progress";
        return;
    }
    auto target = active_;
    if (!save) {
        const auto normalized = std::filesystem::path(path_).lexically_normal().string();
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            if (documents_[i].used && documents_[i].path == normalized && !normalized.empty()) {
                Select(i);
                message_ = "Switched to the already open document";
                return;
            }
        }
        target = documents_.size();
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            if (!documents_[i].used) {
                target = i;
                break;
            }
        }
        if (target == documents_.size()) {
            message_ = "Close a document before opening another (maximum eight)";
            return;
        }
    }
    auto &document = documents_[active_];
    const auto normalized = std::filesystem::path(path_).lexically_normal().string();
    if (save) {
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            if (i != active_ && documents_[i].used && documents_[i].path == normalized) {
                message_ = "That path is open in another document";
                return;
            }
        }
    }
    FileJob job{save ? "save" : "open", path_, save ? document.text : "",
                save && normalized == document.path ? document.stamp : FileStamp{}};
    const auto bytes = nlohmann::json::to_cbor(nlohmann::json(job));
    PrismWorkRequestV1 request{sizeof(request), ++task_,    PRISM_WORK_CRITICAL_V1,
                               1024 * 1024,     128 * 1024, {bytes.data(), bytes.size()},
                               FileWork};
    if (host_->submit_work(host_->context, &request) != PRISM_WORK_ACCEPTED_V1) {
        close_after_save_ = false;
        message_ = "File worker is busy; please try again";
        return;
    }
    busy_ = true;
    saving_ = save;
    pending_document_ = target;
    submitted_text_ = job.text;
    message_ = save ? "Saving… You can continue editing" : "Opening…";
}

void Notepad::Action(std::string_view action)
{
    if (busy_) {
        message_ = "File operation in progress";
        Publish();
        return;
    }
    if (confirm_ && action != "discard" && action != "cancel" && action != "save-close") {
        return;
    }
    if (path_panel_ && action != "apply-path" && action != "cancel") {
        return;
    }

    if (action == "new") {
        New();
    } else if (action == "open-panel" || action == "save-as") {
        ShowPath(action == "save-as");
    } else if (action == "apply-path") {
        Start(path_save_);
    } else if (action == "open") {
        Start(false);
    } else if (action == "save") {
        if (path_.empty()) {
            ShowPath(true);
        } else {
            Start(true);
        }
    } else if (action == "save-close" && confirm_) {
        Select(closing_);
        close_after_save_ = true;
        confirm_ = false;
        if (path_.empty()) {
            ShowPath(true);
        } else {
            Start(true);
        }
    } else if (action == "close") {
        Close(false);
    } else if (action.size() == 7 && action.starts_with("close:") && action.back() >= '0' &&
               action.back() <= '7') {
        const auto index = static_cast<std::size_t>(action.back() - '0');
        if (documents_[index].used) {
            closing_ = index;
            if (documents_[index].Dirty()) {
                confirm_ = true;
            } else {
                Remove(index);
            }
        }
    } else if (action == "discard" && confirm_) {
        Close(true);
    } else if (action == "cancel") {
        confirm_ = path_panel_ = list_ = close_after_save_ = false;
        path_ = documents_[active_].path;
        message_ = "Ready";
    } else if (action == "documents") {
        list_ = !list_;
    } else if (action == "page-previous" || action == "page-next") {
        const auto count = std::count_if(documents_.begin(), documents_.end(),
                                         [](const Document &doc) { return doc.used; });
        const auto pages = static_cast<std::size_t>((count + 1) / 2);
        tab_page_ = (tab_page_ + (action == "page-next" ? 1 : pages - 1)) % pages;
    } else if (action == "previous" || action == "next") {
        for (std::size_t step = 1; step <= documents_.size(); ++step) {
            const auto index = (active_ + (action == "next" ? step : documents_.size() - step)) %
                               documents_.size();
            if (documents_[index].used) {
                Select(index);
                break;
            }
        }
    } else if (action.size() == 8 && action.starts_with("select:") && action.back() >= '0' &&
               action.back() <= '7') {
        Select(action.back() - '0');
    }
    Publish();
}

void Notepad::Edit(std::string_view action, std::string_view text)
{
    if (action == "path") {
        path_ = text;
    } else if (action.size() == 6 && action.starts_with("edit:") && action.back() >= '0' &&
               action.back() <= '7') {
        auto &document = documents_[action.back() - '0'];
        if (document.used) {
            document.text = text;
        }
    }
    Publish();
}

void Notepad::Complete(const PrismWorkCompletionV1 &completion)
{
    if (!busy_ || completion.task_id != task_) {
        return;
    }
    busy_ = false;
    if (completion.status != PRISM_WORK_SUCCEEDED_V1) {
        close_after_save_ = false;
        message_ = "File operation failed or was cancelled; document retained";
        Publish();
        return;
    }
    const auto result = nlohmann::json::from_cbor(completion.result.data,
                                                  completion.result.data + completion.result.size)
                            .get<FileResult>();
    if (!result.error.empty()) {
        close_after_save_ = false;
        message_ = result.error;
        Publish();
        return;
    }
    path_panel_ = false;
    auto &document = documents_[pending_document_];
    document.used = true;
    document.path = result.path;
    document.stamp = result.stamp;
    document.title = std::filesystem::path(result.path).filename().string();
    if (saving_) {
        document.saved = submitted_text_;
    } else {
        document.text = document.saved = result.text;
        Select(pending_document_);
    }
    if (active_ == pending_document_) {
        path_ = document.path;
    }
    message_ = saving_ ? (document.Dirty() ? "Saved snapshot; newer edits are still unsaved"
                                           : "Saved successfully")
                       : "Opened UTF-8 text";
    if (close_after_save_) {
        close_after_save_ = false;
        if (!document.Dirty()) {
            Remove(pending_document_);
        } else {
            closing_ = pending_document_;
            confirm_ = true;
        }
    }
    Publish();
}

bool Notepad::RequestClose()
{
    if (busy_) {
        message_ = "Please wait for the file operation before quitting";
        Publish();
        return false;
    }
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (documents_[i].used && documents_[i].Dirty()) {
            Select(i);
            confirm_ = true;
            closing_ = i;
            message_ =
                "Quit paused: save this document, or discard it, then close the window again";
            Publish();
            return false;
        }
    }
    return true;
}

void Notepad::Publish()
{
    const bool normal = !confirm_ && !path_panel_ && !list_;
    const auto total = std::count_if(documents_.begin(), documents_.end(),
                                     [](const Document &doc) { return doc.used; });
    tab_page_ = std::min(tab_page_, static_cast<std::size_t>((total - 1) / 2));
    std::size_t count = 0;
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        const auto &document = documents_[i];
        const auto suffix = std::to_string(i);
        Boolean("tab_" + suffix, document.used && count / 2 == tab_page_ && normal);
        count += document.used;
        Boolean("active_" + suffix, document.used && i == active_ && normal);
        Boolean("used_" + suffix, document.used);
        Boolean("selected_" + suffix, document.used && i == active_);
        String("text_" + suffix, document.text);
        String("label_" + suffix, (document.Dirty() ? "● " : "") + document.title);
    }
    const auto &document = documents_[active_];
    String("title", (document.Dirty() ? "● " : "") + document.title);
    String("path", path_);
    String("status", message_);
    String("details",
           std::to_string(count) + "/8 documents · " + std::to_string(document.text.size()) +
               " bytes · " +
               std::to_string(1 + std::count(document.text.begin(), document.text.end(), '\n')) +
               " lines" + (document.Dirty() ? " · Unsaved" : ""));
    Boolean("normal", normal);
    Boolean("pages", total > 2);
    Boolean("path_panel", path_panel_);
    String("path_title", path_save_ ? "Save document" : "Open document");
    String("confirm_title", "Save changes to " + documents_[closing_].title + "?");
    String("summary",
           std::to_string(1 + std::count(document.text.begin(), document.text.end(), '\n')) +
               " lines · " + (document.Dirty() ? "Unsaved" : "UTF-8"));
    Boolean("confirm", confirm_);
    Boolean("documents", list_);
    Boolean("busy", busy_);
    Number("list_opacity", list_ ? 1 : 0);
    Number("list_offset", list_ ? 0 : 6);
    Number("confirm_opacity", confirm_ ? 1 : 0);
    Number("confirm_offset", confirm_ ? 0 : 4);
    Number("activity_opacity", busy_ ? 1 : 0);
}
} // namespace prism::notepad
