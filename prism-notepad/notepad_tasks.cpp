#include "notepad.hpp"
#include <cstddef>
#include <filesystem>

namespace prism::notepad {
namespace {
PrismTaskStringViewV1 View(std::string_view text)
{
    return {text.data(), text.size()};
}
} // namespace

bool Notepad::SupportsTasks(std::uint32_t capability) const
{
    return host_ &&
           host_->struct_size >=
               offsetof(PrismHostApiV1, cancel_task) + sizeof(host_->cancel_task) &&
           host_->task_capabilities && host_->request_task && host_->cancel_task &&
           (host_->task_capabilities(host_->context) & capability);
}

bool Notepad::SupportsCloseContinuation() const
{
    return host_ &&
           host_->struct_size >=
               offsetof(PrismHostApiV1, complete_close) + sizeof(host_->complete_close) &&
           host_->complete_close;
}

bool Notepad::RequestFile(bool save, std::string_view hint)
{
    ClearFeedback();
    const auto capability = save ? PRISM_TASK_CAP_SAVE_FILE_V1 : PRISM_TASK_CAP_OPEN_FILE_V1;
    if (!SupportsTasks(capability)) {
        message_ = "File selection is unavailable; document retained";
        AbortClosing();
        ShowFeedback(PRISM_FEEDBACK_ERROR_V1, "File selection unavailable", message_);
        return false;
    }

    const auto &document = documents_[active_];
    const auto path = hint.empty() ? document.path : std::string(hint);
    const auto directory =
        path.empty() ? std::string{} : std::filesystem::path(path).parent_path().string();
    const auto name = !save          ? std::string{}
                      : path.empty() ? std::string{"Untitled.txt"}
                                     : std::filesystem::path(path).filename().string();
    PrismFileTaskOptionsV1 options{};
    options.struct_size = sizeof(options);
    options.initial_directory = View(directory);
    options.suggested_name = View(name);
    PrismTaskRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = save ? PRISM_TASK_SAVE_FILE_V1 : PRISM_TASK_OPEN_FILE_V1;
    request.title = View(save ? "Save document" : "Open text file");
    request.file = &options;

    const auto id = host_->request_task(host_->context, &request);
    if (!id) {
        message_ = "File selection could not start; document retained";
        AbortClosing();
        ShowFeedback(PRISM_FEEDBACK_ERROR_V1, "File selection failed", message_);
        return false;
    }

    request_id_ = id;
    task_purpose_ = save ? TaskPurpose::SaveAs : TaskPurpose::Open;
    pending_document_ = active_;
    list_ = false;
    message_ = save ? "Choose where to save this document" : "Choose a UTF-8 text file";
    return true;
}

bool Notepad::RequestConfirmation(std::size_t index)
{
    ClearFeedback();
    if (!SupportsTasks(PRISM_TASK_CAP_CONFIRMATION_V1)) {
        message_ = "Unsaved confirmation is unavailable; document retained";
        AbortClosing();
        ShowFeedback(PRISM_FEEDBACK_ERROR_V1, "Confirmation unavailable", message_);
        return false;
    }

    Select(index);
    closing_ = index;
    const auto message = "Save changes to “" + documents_[index].title + "”?";
    const PrismTaskChoiceV1 choices[] = {
        {sizeof(PrismTaskChoiceV1), 1, View("Save"), PRISM_TASK_CHOICE_PRIMARY_V1},
        {sizeof(PrismTaskChoiceV1), 2, View("Discard"), PRISM_TASK_CHOICE_DESTRUCTIVE_V1}};
    const PrismTaskRequestV1 request{sizeof(request),
                                     PRISM_TASK_CONFIRMATION_V1,
                                     View("Unsaved changes"),
                                     View(message),
                                     choices,
                                     2,
                                     nullptr};
    const auto id = host_->request_task(host_->context, &request);
    if (!id) {
        message_ = "Unsaved confirmation could not start; document retained";
        AbortClosing();
        ShowFeedback(PRISM_FEEDBACK_ERROR_V1, "Confirmation failed", message_);
        return false;
    }

    request_id_ = id;
    task_purpose_ = TaskPurpose::Confirm;
    message_ = "Choose Save, Discard or Cancel";
    return true;
}

void Notepad::TaskComplete(const PrismTaskResultV1 &result)
{
    if (!request_id_ || result.request_id != request_id_) {
        return;
    }

    const auto purpose = task_purpose_;
    request_id_ = 0;
    task_purpose_ = TaskPurpose::None;
    if (purpose == TaskPurpose::Abandoned) {
        Publish();
        return;
    }
    if (result.struct_size < offsetof(PrismTaskResultV1, diagnostic) + sizeof(result.diagnostic) ||
        result.outcome != PRISM_TASK_SUCCEEDED_V1) {
        message_ = result.outcome == PRISM_TASK_CANCELLED_V1
                       ? "Cancelled; document retained"
                       : "File task failed; document retained";
        AbortClosing();
        ShowFeedback(result.outcome == PRISM_TASK_CANCELLED_V1 ? PRISM_FEEDBACK_INFO_V1
                                                               : PRISM_FEEDBACK_ERROR_V1,
                     result.outcome == PRISM_TASK_CANCELLED_V1 ? "Cancelled" : "File task failed",
                     message_);
        Publish();
        return;
    }

    if (purpose == TaskPurpose::Confirm) {
        if (result.choice_id == 1 && closing_ < documents_.size()) {
            Select(closing_);
            if (documents_[closing_].path.empty()) {
                RequestFile(true);
            } else {
                StartWork(true, documents_[closing_].path);
            }
        } else if (result.choice_id == 2 && closing_ < documents_.size()) {
            const auto index = closing_;
            closing_ = NoDocument;
            if (close_id_) {
                discard_approved_[index] = true;
                discard_snapshots_[index] = documents_[index].text;
                ContinueOwnerClose();
            } else {
                Remove(index);
                message_ = "Document closed";
            }
        } else {
            message_ = "Invalid confirmation result; document retained";
            AbortClosing();
        }
    } else if (result.struct_size < offsetof(PrismTaskResultV1, overwrite_approved) +
                                        sizeof(result.overwrite_approved) ||
               !result.file_path.data || !result.file_path.size || result.file_path.size > 4096 ||
               result.overwrite_approved > 1) {
        message_ = "Invalid file selection result; document retained";
        AbortClosing();
    } else {
        const std::string path(result.file_path.data, result.file_path.size);
        StartWork(purpose == TaskPurpose::SaveAs, path, result.overwrite_approved != 0);
    }
    Publish();
}

bool Notepad::RequestLegacyClose()
{
    ClearFeedback();
    if (busy_ || request_id_ || close_id_ || closing_ != NoDocument) {
        return false;
    }
    for (const auto &document : documents_) {
        if (document.used && document.Dirty()) {
            message_ = "This Host cannot continue asynchronous closing; save documents first";
            Publish();
            return false;
        }
    }
    return true;
}

int32_t Notepad::RequestClose(const PrismCloseRequestV1 &request)
{
    ClearFeedback();
    if (request.struct_size < sizeof(request) || !request.request_id || request_id_ || close_id_ ||
        closing_ != NoDocument) {
        return PRISM_CLOSE_REJECT_V1;
    }

    bool dirty = false;
    for (const auto &document : documents_) {
        dirty |= document.used && document.Dirty();
    }
    if (!dirty && !busy_) {
        return PRISM_CLOSE_ACCEPT_V1;
    }
    if (!SupportsCloseContinuation()) {
        message_ = "Asynchronous closing is unavailable; documents retained";
        ShowFeedback(PRISM_FEEDBACK_ERROR_V1, "Could not close", message_);
        Publish();
        return PRISM_CLOSE_REJECT_V1;
    }

    close_id_ = request.request_id;
    discard_approved_.fill(false);
    for (auto &snapshot : discard_snapshots_) {
        snapshot.clear();
    }
    list_ = false;
    if (busy_) {
        message_ = "Closing will continue after the file operation";
    } else {
        ContinueOwnerClose();
    }
    Publish();
    return PRISM_CLOSE_DEFER_V1;
}

void Notepad::ContinueOwnerClose()
{
    if (!close_id_ || busy_ || request_id_) {
        return;
    }

    closing_ = NoDocument;
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        const auto &document = documents_[i];
        if (document.used && document.Dirty() &&
            (!discard_approved_[i] || discard_snapshots_[i] != document.text)) {
            RequestConfirmation(i);
            return;
        }
    }
    FinishOwnerClose(true);
}

void Notepad::FinishOwnerClose(bool accept)
{
    const auto id = close_id_;
    close_id_ = 0;
    closing_ = NoDocument;
    discard_approved_.fill(false);
    for (auto &snapshot : discard_snapshots_) {
        snapshot.clear();
    }

    if (id && SupportsCloseContinuation()) {
        if (host_->complete_close(host_->context, id,
                                  accept ? PRISM_CLOSE_ACCEPT_V1 : PRISM_CLOSE_REJECT_V1) !=
            PRISM_CLOSE_COMPLETED_V1) {
            message_ = "Close continuation was rejected; documents retained";
        }
    }
}

void Notepad::Recover() noexcept
{
    ClearFeedback();
    const auto close = close_id_;
    close_id_ = 0;
    closing_ = NoDocument;
    discard_approved_.fill(false);
    for (auto &snapshot : discard_snapshots_) {
        snapshot.clear();
    }

    // An accepted provider request must drain its terminal callback before
    // another request starts. Its result is no longer a business instruction.
    if (request_id_) {
        task_purpose_ = TaskPurpose::Abandoned;
        try {
            if (host_->struct_size >=
                    offsetof(PrismHostApiV1, cancel_task) + sizeof(host_->cancel_task) &&
                host_->cancel_task) {
                host_->cancel_task(host_->context, request_id_);
            }
        } catch (...) {
        }
    }
    if (close && SupportsCloseContinuation()) {
        try {
            host_->complete_close(host_->context, close, PRISM_CLOSE_REJECT_V1);
        } catch (...) {
        }
    }

    // Accepted work keeps its ID and exact submitted snapshot until completion.
    // Best-effort publication restores controls after a transient Host failure.
    try {
        message_ = "Task failed";
        Publish();
    } catch (...) {
    }
}

void Notepad::AbortClosing()
{
    closing_ = NoDocument;
    if (close_id_) {
        FinishOwnerClose(false);
    }
}
} // namespace prism::notepad
