#include "prism/runtime/text_buffer.hpp"
#include <algorithm>
#include <cstdint>

namespace prism::runtime {
bool TextBuffer::Valid(std::string_view text)
{
    if (text.size() > MaxBytes) {
        return false;
    }
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) {
            if (first < 32 && first != '\n' && first != '\r' && first != '\t') {
                return false;
            }
            if (first == 127) {
                return false;
            }
            continue;
        }
        unsigned remaining = first >= 0xc2 && first <= 0xdf   ? 1
                             : first >= 0xe0 && first <= 0xef ? 2
                             : first >= 0xf0 && first <= 0xf4 ? 3
                                                              : 0;
        if (!remaining || i + remaining > text.size()) {
            return false;
        }
        const unsigned minimum = remaining == 1 ? 0x80 : remaining == 2 ? 0x800 : 0x10000;
        std::uint32_t value = first & (0x7f >> remaining);
        while (remaining--) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (byte & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

std::size_t TextBuffer::Next(std::string_view text, std::size_t offset)
{
    if (offset + 1 < text.size() && text[offset] == '\r' && text[offset + 1] == '\n') {
        return offset + 2;
    }
    if (offset < text.size()) {
        ++offset;
    }
    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) {
        ++offset;
    }
    return offset;
}

std::size_t TextBuffer::Previous(std::string_view text, std::size_t offset)
{
    offset = std::min(offset, text.size());
    if (offset >= 2 && text[offset - 2] == '\r' && text[offset - 1] == '\n') {
        return offset - 2;
    }
    if (offset) {
        --offset;
    }
    while (offset && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) {
        --offset;
    }
    return offset;
}

bool TextBuffer::Assign(std::string text)
{
    if (!Valid(text) || text == text_) {
        return false;
    }
    text_ = std::move(text);
    cursor_ = anchor_ = 0;
    undo_.clear();
    redo_.clear();
    scroll_x = scroll_y = 0;
    reveal = true;
    return true;
}

TextBuffer::Revision TextBuffer::Current() const
{
    return {text_, cursor_, anchor_};
}

void TextBuffer::Restore(Revision value)
{
    text_ = std::move(value.text);
    cursor_ = value.cursor;
    anchor_ = value.anchor;
    reveal = true;
}

void TextBuffer::Remember()
{
    if (undo_.size() == 64) {
        undo_.erase(undo_.begin());
    }
    undo_.push_back(Current());
    redo_.clear();
}

void TextBuffer::Select(std::size_t offset, bool extend)
{
    cursor_ = std::min(offset, text_.size());
    while (cursor_ && cursor_ < text_.size() &&
           (static_cast<unsigned char>(text_[cursor_]) & 0xc0) == 0x80) {
        --cursor_;
    }
    if (cursor_ && cursor_ < text_.size() && text_[cursor_] == '\n' && text_[cursor_ - 1] == '\r') {
        --cursor_;
    }
    if (!extend) {
        anchor_ = cursor_;
    }
    reveal = true;
}

std::string TextBuffer::Selection() const
{
    return text_.substr(std::min(cursor_, anchor_),
                        std::max(cursor_, anchor_) - std::min(cursor_, anchor_));
}

bool TextBuffer::Insert(std::string_view text)
{
    const auto start = std::min(cursor_, anchor_);
    const auto end = std::max(cursor_, anchor_);
    if (!Valid(text) || text_.size() - (end - start) + text.size() > MaxBytes ||
        (text.empty() && start == end)) {
        return false;
    }
    Remember();
    text_.replace(start, end - start, text);
    cursor_ = anchor_ = start + text.size();
    reveal = true;
    return true;
}

bool TextBuffer::Key(unsigned key, bool control, bool shift, bool multiline)
{
    if (control && (key == 0x1d || key == 0x1c)) {
        const bool redo = key == 0x1c || shift;
        auto &from = redo ? redo_ : undo_;
        auto &to = redo ? undo_ : redo_;
        if (from.empty()) {
            return false;
        }
        to.push_back(Current());
        Restore(std::move(from.back()));
        from.pop_back();
        return true;
    }
    if (control && key == 0x04) {
        anchor_ = 0;
        cursor_ = text_.size();
        reveal = true;
        return true;
    }
    if (key == 0x2a || key == 0x4c) {
        if (cursor_ == anchor_) {
            anchor_ = key == 0x2a ? Previous(text_, cursor_) : Next(text_, cursor_);
        }
        return Insert("");
    }
    if (key == 0x28 && multiline && !control) {
        return Insert(text_.find("\r\n") == std::string::npos ? "\n" : "\r\n");
    }
    std::size_t target = cursor_;
    const auto start = cursor_ ? text_.rfind('\n', cursor_ - 1) : std::string::npos;
    const auto line = start == std::string::npos ? 0 : start + 1;
    const auto end = text_.find('\n', cursor_);
    if (key == 0x50) {
        target =
            !shift && anchor_ != cursor_ ? std::min(anchor_, cursor_) : Previous(text_, cursor_);
    } else if (key == 0x4f) {
        target = !shift && anchor_ != cursor_ ? std::max(anchor_, cursor_) : Next(text_, cursor_);
    } else if (key == 0x4a) {
        target = control ? 0 : line;
    } else if (key == 0x4d) {
        target = control || end == std::string::npos ? text_.size() : end;
    } else if (key == 0x52 || key == 0x51) {
        std::size_t column = 0;
        for (auto i = line; i < cursor_; i = Next(text_, i)) {
            ++column;
        }
        if (key == 0x52) {
            const auto prior = line > 1 ? text_.rfind('\n', line - 2) : std::string::npos;
            target = line ? (prior == std::string::npos ? 0 : prior + 1) : 0;
        } else {
            target = end == std::string::npos ? text_.size() : end + 1;
        }
        while (column-- && target < text_.size() && text_[target] != '\n' &&
               text_[target] != '\r') {
            target = Next(text_, target);
        }
    } else {
        return false;
    }
    Select(target, shift);
    return true;
}
} // namespace prism::runtime
