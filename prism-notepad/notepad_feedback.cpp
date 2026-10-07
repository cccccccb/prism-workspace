#include "notepad.hpp"

#include "prism/runtime/text_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace prism::notepad {
namespace {
PrismFeedbackStringViewV1 View(std::string_view text)
{
    return {text.data(), text.size()};
}

std::string FeedbackText(std::string_view text, std::size_t limit)
{
    if (!runtime::TextBuffer::Valid(text)) {
        return "Details unavailable; document retained";
    }

    std::string clean;
    clean.reserve(std::min(text.size(), limit));
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        const std::size_t bytes = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        std::uint32_t scalar = first & (bytes == 1   ? 0x7f
                                        : bytes == 2 ? 0x1f
                                        : bytes == 3 ? 0xf
                                                     : 7);
        for (std::size_t i = 1; i < bytes; ++i) {
            scalar = (scalar << 6) | (static_cast<unsigned char>(text[offset + i]) & 0x3f);
        }
        const bool control = (scalar < 0x20 && scalar != '\n') ||
                             (scalar >= 0x7f && scalar <= 0x9f) || scalar == 0x2028 ||
                             scalar == 0x2029;
        const auto size = control ? 1 : bytes;
        if (clean.size() + size > limit - 3) {
            clean += "…";
            break;
        }
        if (control) {
            clean += ' ';
        } else {
            clean.append(text.substr(offset, bytes));
        }
        offset += bytes;
    }
    return clean;
}
} // namespace

bool Notepad::SupportsFeedback() const noexcept
{
    try {
        return host_ &&
               host_->struct_size >=
                   offsetof(PrismHostApiV1, dismiss_feedback) + sizeof(host_->dismiss_feedback) &&
               host_->feedback_capabilities && host_->show_feedback && host_->dismiss_feedback &&
               (host_->feedback_capabilities(host_->context) & PRISM_FEEDBACK_CAP_OWNER_V1);
    } catch (...) {
        return false;
    }
}

void Notepad::ClearFeedback() noexcept
{
    const auto id = feedback_.id;
    feedback_.id = 0;
    feedback_.generation = 0;
    feedback_.document = NoDocument;
    feedback_.purpose = FeedbackPurpose::None;
    feedback_.document_path.clear();
    feedback_.operation_path.clear();

    if (!id || !host_ ||
        host_->struct_size <
            offsetof(PrismHostApiV1, dismiss_feedback) + sizeof(host_->dismiss_feedback) ||
        !host_->dismiss_feedback) {
        return;
    }
    try {
        host_->dismiss_feedback(host_->context, id);
    } catch (...) {
    }
}

void Notepad::ShowFeedback(std::uint32_t kind, std::string_view title, std::string_view message,
                           FeedbackPurpose purpose, std::string_view operation_path) noexcept
{
    ClearFeedback();
    if (!SupportsFeedback()) {
        return;
    }

    try {
        FeedbackState prepared;
        prepared.document = active_;
        prepared.generation = documents_[active_].generation;
        prepared.document_path = documents_[active_].path;
        prepared.operation_path = operation_path;
        prepared.purpose = purpose;
        const auto text = FeedbackText(message, 2048);
        const PrismFeedbackActionV1 actions[] = {
            {sizeof(PrismFeedbackActionV1), 1,
             View(purpose == FeedbackPurpose::SaveFailure ? "Change location" : "Retry")},
            {sizeof(PrismFeedbackActionV1), 2, View("Choose file")}};
        const auto count = purpose == FeedbackPurpose::None          ? 0u
                           : purpose == FeedbackPurpose::SaveFailure ? 1u
                                                                     : 2u;
        const PrismFeedbackRequestV1 request{sizeof(request),
                                             kind,
                                             View(title),
                                             View(text),
                                             count ? actions : nullptr,
                                             count,
                                             kind == PRISM_FEEDBACK_ERROR_V1 ? 0u : 4000u};
        const auto id = host_->show_feedback(host_->context, &request);
        if (!id) {
            return;
        }

        prepared.id = id;
        feedback_ = std::move(prepared);
    } catch (...) {
        // Feedback is optional; its failure cannot roll back saved state or
        // reinterpret an existing close/file operation as business failure.
    }
}

void Notepad::ShowFileFailure(bool save, std::string_view reason, std::string_view path) noexcept
{
    try {
        const auto detail = std::string(reason) + "\nYour document is retained.";
        ShowFeedback(PRISM_FEEDBACK_ERROR_V1,
                     save ? "Could not save document" : "Could not open file", detail,
                     save ? FeedbackPurpose::SaveFailure : FeedbackPurpose::OpenFailure, path);
    } catch (...) {
    }
}

void Notepad::ShowSavedFeedback() noexcept
{
    try {
        const auto &document = documents_[active_];
        const auto receipt = document.title + "\n" + message_;
        ShowFeedback(PRISM_FEEDBACK_SUCCESS_V1, document.Dirty() ? "Snapshot saved" : "Saved",
                     receipt);
    } catch (...) {
    }
}

void Notepad::FeedbackAction(const PrismFeedbackActionEventV1 &event)
{
    if (event.struct_size < sizeof(event) || !feedback_.id || event.request_id != feedback_.id ||
        feedback_.document != active_ || !documents_[active_].used ||
        feedback_.generation != documents_[active_].generation ||
        feedback_.document_path != documents_[active_].path || busy_ || request_id_ || close_id_ ||
        closing_ != NoDocument) {
        return;
    }

    const auto purpose = feedback_.purpose;
    if ((purpose == FeedbackPurpose::SaveFailure && event.action_id != 1) ||
        (purpose == FeedbackPurpose::OpenFailure && event.action_id != 1 && event.action_id != 2) ||
        purpose == FeedbackPurpose::None) {
        return;
    }
    const auto path = feedback_.operation_path;
    ClearFeedback();

    if (purpose == FeedbackPurpose::SaveFailure) {
        RequestFile(true, path);
    } else if (event.action_id == 1) {
        StartWork(false, path);
    } else {
        RequestFile(false, path);
    }
    Publish();
}
} // namespace prism::notepad
