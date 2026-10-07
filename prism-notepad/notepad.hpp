#pragma once
#include "files.hpp"
#include "prism/contracts/app_close.h"
#include "prism/contracts/app_feedback.h"
#include <array>

namespace prism::notepad {
struct Document {
    bool used{};
    std::uint64_t generation{};
    std::string title, path, text, saved;
    FileStamp stamp;

    bool Dirty() const
    {
        return text != saved;
    }
};

class Notepad {
public:
    explicit Notepad(const PrismHostApiV1 *host);
    void Action(std::string_view);
    void Edit(std::string_view, std::string_view);
    void Complete(const PrismWorkCompletionV1 &);
    void TaskComplete(const PrismTaskResultV1 &);
    void Publish();
    int32_t RequestClose(const PrismCloseRequestV1 &);
    bool RequestLegacyClose();
    void Recover() noexcept;
    void FeedbackAction(const PrismFeedbackActionEventV1 &);

private:
    enum class TaskPurpose { None, Open, SaveAs, Confirm, Abandoned };
    static constexpr std::size_t NoDocument = 8;
    enum class FeedbackPurpose { None, SaveFailure, OpenFailure };

    struct FeedbackState {
        std::uint64_t id{}, generation{};
        std::size_t document{NoDocument};
        FeedbackPurpose purpose{FeedbackPurpose::None};
        std::string document_path, operation_path;
    };

    void New();
    void CloseDocument(std::size_t);
    void Remove(std::size_t);
    void Select(std::size_t);
    bool RequestFile(bool save, std::string_view hint = {});
    bool RequestConfirmation(std::size_t);
    bool SupportsTasks(std::uint32_t capability) const;
    bool SupportsCloseContinuation() const;
    bool StartWork(bool save, std::string path, bool overwrite = false);
    void FinishSavedDocument(std::size_t);
    void ContinueOwnerClose();
    void FinishOwnerClose(bool accept);
    void AbortClosing();
    void String(std::string_view, std::string_view);
    void Number(std::string_view, double);
    void Boolean(std::string_view, bool);
    bool SupportsFeedback() const noexcept;
    void ClearFeedback() noexcept;
    void ShowFeedback(std::uint32_t kind, std::string_view title, std::string_view message,
                      FeedbackPurpose purpose = FeedbackPurpose::None,
                      std::string_view operation_path = {}) noexcept;
    void ShowFileFailure(bool save, std::string_view reason, std::string_view path) noexcept;
    void ShowSavedFeedback() noexcept;

    const PrismHostApiV1 *host_;
    std::array<Document, 8> documents_;
    std::array<bool, 8> discard_approved_{};
    std::array<std::string, 8> discard_snapshots_;
    std::size_t active_{}, pending_document_{}, closing_{NoDocument}, tab_page_{};
    unsigned untitled_{};
    std::uint64_t work_id_{}, request_id_{}, close_id_{}, document_id_{};
    FeedbackState feedback_;
    TaskPurpose task_purpose_{TaskPurpose::None};
    bool busy_{}, saving_{}, list_{};
    std::string message_, submitted_text_, submitted_path_;
};
} // namespace prism::notepad
