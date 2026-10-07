#include "notepad.hpp"
#include "prism/runtime/text_buffer.hpp"
#include <algorithm>
#include <limits>

namespace prism::notepad {
Notepad::Notepad(const PrismHostApiV1 *host) : host_(host)
{
    New();
    message_ = "Ready";
    Publish();
}

void Notepad::Select(std::size_t index)
{
    if (index >= documents_.size() || !documents_[index].used) {
        return;
    }

    ClearFeedback();
    active_ = index;
    std::size_t rank = 0;
    for (std::size_t i = 0; i < index; ++i) {
        rank += documents_[i].used;
    }
    tab_page_ = rank / 2;
    list_ = false;
}

void Notepad::New()
{
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (!documents_[i].used) {
            if (document_id_ == std::numeric_limits<std::uint64_t>::max()) {
                message_ = "Document identity exhausted";
                return;
            }
            documents_[i] = {};
            documents_[i].generation = ++document_id_;
            documents_[i].used = true;
            documents_[i].title = "Untitled " + std::to_string(++untitled_);
            Select(i);
            message_ = "New document";
            return;
        }
    }
    message_ = "Eight documents are open; close one before creating another";
}

void Notepad::CloseDocument(std::size_t index)
{
    if (index >= documents_.size() || !documents_[index].used) {
        return;
    }

    ClearFeedback();
    closing_ = index;
    if (documents_[index].Dirty()) {
        RequestConfirmation(index);
    } else {
        Remove(index);
    }
}

void Notepad::Remove(std::size_t index)
{
    ClearFeedback();
    documents_[index] = {};
    // Reset the stable editor's undo history before reusing the slot.
    String("text_" + std::to_string(index), " ");
    String("text_" + std::to_string(index), "");
    closing_ = NoDocument;

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

void Notepad::Action(std::string_view action)
{
    if (busy_ || request_id_ || close_id_ || closing_ != NoDocument) {
        return;
    }

    if (action == "new") {
        New();
    } else if (action == "open-panel" || action == "open") {
        RequestFile(false);
    } else if (action == "save-as") {
        RequestFile(true);
    } else if (action == "save") {
        if (documents_[active_].path.empty()) {
            RequestFile(true);
        } else {
            StartWork(true, documents_[active_].path);
        }
    } else if (action == "close") {
        CloseDocument(active_);
    } else if (action.size() == 7 && action.starts_with("close:") && action.back() >= '0' &&
               action.back() <= '7') {
        CloseDocument(static_cast<std::size_t>(action.back() - '0'));
    } else if (action == "documents") {
        list_ = !list_;
    } else if (action == "cancel") {
        list_ = false;
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
        Select(static_cast<std::size_t>(action.back() - '0'));
    }
    Publish();
}

void Notepad::Edit(std::string_view action, std::string_view text)
{
    if (action.size() != 6 || !action.starts_with("edit:") || action.back() < '0' ||
        action.back() > '7' || !runtime::TextBuffer::Valid(text)) {
        return;
    }

    const auto index = static_cast<std::size_t>(action.back() - '0');
    const bool saving_editor = busy_ && saving_ && index == pending_document_;
    if (request_id_ || (!saving_editor && (busy_ || close_id_ || closing_ != NoDocument)) ||
        !documents_[index].used) {
        return;
    }

    documents_[index].text = text;
    Publish();
}
} // namespace prism::notepad
