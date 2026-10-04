#pragma once
#include "files.hpp"
#include <array>

namespace prism::notepad {
struct Document {
    bool used{};
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
    void Publish();
    bool RequestClose();

private:
    void New();
    void Close(bool discard);
    void Remove(std::size_t index);
    void ShowPath(bool save);
    void Start(bool save);
    void Select(std::size_t);
    void String(std::string_view, std::string_view);
    void Number(std::string_view, double);
    void Boolean(std::string_view, bool);
    const PrismHostApiV1 *host_;
    std::array<Document, 8> documents_;
    std::size_t active_{}, pending_document_{}, closing_{}, tab_page_{};
    unsigned untitled_{};
    std::uint64_t task_{};
    bool busy_{}, saving_{}, confirm_{}, list_{};
    bool path_panel_{}, path_save_{}, close_after_save_{};
    std::string path_, message_, submitted_text_;
};
} // namespace prism::notepad
