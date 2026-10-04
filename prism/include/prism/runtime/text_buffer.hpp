#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace prism::runtime {
// UTF-8 byte offsets always lie on scalar boundaries. History is bounded.
class TextBuffer {
public:
    static constexpr std::size_t MaxBytes = 48 * 1024;
    static bool Valid(std::string_view text);
    static std::size_t Next(std::string_view text, std::size_t offset);
    static std::size_t Previous(std::string_view text, std::size_t offset);
    bool Assign(std::string text);
    bool Insert(std::string_view text);
    bool Key(unsigned key, bool control, bool shift, bool multiline);
    void Select(std::size_t offset, bool extend = false);

    const std::string &Text() const
    {
        return text_;
    }

    std::size_t Cursor() const
    {
        return cursor_;
    }

    std::size_t Anchor() const
    {
        return anchor_;
    }

    std::string Selection() const;
    double scroll_x{}, scroll_y{};
    bool reveal{true};

private:
    struct Revision {
        std::string text;
        std::size_t cursor{}, anchor{};
    };

    Revision Current() const;
    void Restore(Revision value);
    void Remember();
    std::string text_;
    std::size_t cursor_{}, anchor_{};
    std::vector<Revision> undo_, redo_;
};
} // namespace prism::runtime
