#include "notepad.hpp"
#include "prism/runtime/text_buffer.hpp"
#include <filesystem>
#include <limits>

namespace prism::notepad {
bool Notepad::StartWork(bool save, std::string path, bool overwrite)
{
    ClearFeedback();
    if (busy_ || path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos ||
        !runtime::TextBuffer::Valid(path) || !std::filesystem::path(path).is_absolute() ||
        std::filesystem::path(path).lexically_normal().string() != path || (!save && overwrite)) {
        message_ = "Invalid file path; document retained";
        AbortClosing();
        ShowFeedback(PRISM_FEEDBACK_ERROR_V1, "Invalid file path", message_);
        return false;
    }

    auto target = active_;
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (!documents_[i].used || documents_[i].path != path) {
            continue;
        }
        if (!save) {
            Select(i);
            message_ = "Switched to the already open document";
            return true;
        }
        if (i != active_) {
            message_ = "That path is open in another document";
            AbortClosing();
            ShowFileFailure(true, message_, path);
            return false;
        }
    }
    if (!save) {
        target = NoDocument;
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            if (!documents_[i].used) {
                target = i;
                break;
            }
        }
        if (target == NoDocument) {
            message_ = "Close a document before opening another (maximum eight)";
            ShowFeedback(PRISM_FEEDBACK_ERROR_V1, "Document limit reached", message_);
            return false;
        }
    }

    const auto &document = documents_[active_];
    const auto expected = save && path == document.path ? document.stamp : FileStamp{};
    FileJob job{save ? "save" : "open", std::move(path), save ? document.text : "", expected,
                overwrite};
    const auto bytes = nlohmann::json::to_cbor(nlohmann::json(job));
    std::string submitted = job.text;
    std::string selected_path = job.path;
    std::string progress = save ? "Saving… You can continue editing" : "Opening…";
    if (work_id_ == std::numeric_limits<std::uint64_t>::max()) {
        message_ = "File work identity exhausted; document retained";
        AbortClosing();
        ShowFeedback(PRISM_FEEDBACK_ERROR_V1, "File operation unavailable", message_);
        return false;
    }
    const auto id = ++work_id_;
    const PrismWorkRequestV1 request{sizeof(request), id,         PRISM_WORK_CRITICAL_V1,
                                     1024 * 1024,     128 * 1024, {bytes.data(), bytes.size()},
                                     FileWork};
    if (host_->submit_work(host_->context, &request) != PRISM_WORK_ACCEPTED_V1) {
        message_ = "File worker is busy; please try again";
        AbortClosing();
        ShowFileFailure(save, message_, job.path);
        return false;
    }

    busy_ = true;
    saving_ = save;
    pending_document_ = target;
    submitted_text_.swap(submitted);
    submitted_path_.swap(selected_path);
    message_.swap(progress);
    return true;
}

void Notepad::Complete(const PrismWorkCompletionV1 &completion)
{
    if (!busy_ || completion.task_id != work_id_) {
        return;
    }

    busy_ = false;
    if (completion.struct_size < sizeof(completion) ||
        completion.status != PRISM_WORK_SUCCEEDED_V1 || !completion.result.data ||
        !completion.result.size || completion.result.size > 128 * 1024) {
        message_ = "File operation failed or was cancelled; document retained";
        AbortClosing();
        if (completion.status == PRISM_WORK_CANCELLED_V1) {
            ShowFeedback(PRISM_FEEDBACK_INFO_V1, "Cancelled", message_);
        } else {
            ShowFileFailure(saving_, message_, submitted_path_);
        }
        Publish();
        return;
    }

    FileResult result;
    try {
        result = nlohmann::json::from_cbor(completion.result.data,
                                           completion.result.data + completion.result.size)
                     .get<FileResult>();
        if (result.error.empty() &&
            (result.path.empty() || result.path.size() > 4096 ||
             result.path.find('\0') != std::string::npos ||
             !std::filesystem::path(result.path).is_absolute() ||
             !runtime::TextBuffer::Valid(result.path) || !result.stamp.exists ||
             (!saving_ && !runtime::TextBuffer::Valid(result.text)))) {
            result.error = "Invalid file operation result; document retained";
        }
    } catch (...) {
        result.error = "Invalid file operation result; document retained";
    }
    if (!result.error.empty()) {
        message_ = result.error;
        AbortClosing();
        ShowFileFailure(saving_, message_, submitted_path_);
        Publish();
        return;
    }

    if (!saving_ && document_id_ == std::numeric_limits<std::uint64_t>::max()) {
        message_ = "Document identity exhausted; existing documents retained";
        AbortClosing();
        ShowFileFailure(false, message_, submitted_path_);
        Publish();
        return;
    }
    auto &document = documents_[pending_document_];
    std::string title = std::filesystem::path(result.path).filename().string();
    std::string saved = saving_ ? submitted_text_ : result.text;
    std::string feedback =
        saving_ ? (document.text != saved ? "Saved snapshot; newer edits are still unsaved"
                                          : "Saved successfully")
                : "Opened UTF-8 text";

    document.used = true;
    document.path.swap(result.path);
    document.stamp = result.stamp;
    document.title.swap(title);
    document.saved.swap(saved);
    if (!saving_) {
        document.generation = ++document_id_;
        document.text.swap(result.text);
        Select(pending_document_);
    }
    message_.swap(feedback);

    const bool closing = close_id_ || closing_ != NoDocument;
    FinishSavedDocument(pending_document_);
    if (!closing && saving_) {
        ShowSavedFeedback();
    }
    Publish();
}

void Notepad::FinishSavedDocument(std::size_t index)
{
    if (close_id_) {
        closing_ = NoDocument;
        ContinueOwnerClose();
    } else if (saving_ && closing_ == index) {
        if (documents_[index].Dirty()) {
            RequestConfirmation(index);
        } else {
            Remove(index);
            message_ = "Document saved and closed";
        }
    }
}
} // namespace prism::notepad
