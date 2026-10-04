#include "prism/runtime/scene_snapshot.hpp"
#include "scene_p.hpp"
#include <limits>

namespace prism::runtime {
void Scene::PrepareTextVisual(Node &node, SnapshotNode &item)
{
    auto &editor = node.editor;
    const auto &text = editor.Text();
    const double padding = node.style.padding;
    const double height = std::max(1.0, node.style.font_size * 1.5);
    const double space = std::max(1.0, shaper_(" ", node.style.font_size).width);
    const double width = std::max(1.0, node.bounds.width - 2 * padding);
    const double view_height = std::max(height, node.bounds.height - 2 * padding);
    std::vector<contracts::LogicalRect> cells;
    cells.reserve(text.size() + 1);
    std::vector<std::size_t> offsets;
    std::map<std::string, ShapedText> shapes;
    double x = 0, y = 0, maximum = 0;
    contracts::LogicalRect caret{0, 0, 1.5, height};

    for (std::size_t i = 0;;) {
        const auto next = TextBuffer::Next(text, i);
        const auto scalar = std::string_view(text).substr(i, next - i);
        const bool newline = scalar == "\n" || scalar == "\r\n";
        if (!scalar.empty() && !newline && scalar != "\r" && scalar != "\t" &&
            !shapes.contains(std::string(scalar))) {
            shapes.emplace(std::string(scalar), shaper_(scalar, node.style.font_size));
        }
        const double advance = scalar.empty() || newline || scalar == "\r" ? space
                               : scalar == "\t"
                                   ? space * 4
                                   : std::max(1.0, shapes.at(std::string(scalar)).width);
        cells.push_back({x, y, advance, height});
        offsets.push_back(i);
        if (i == editor.Cursor()) {
            caret = {x, y, 1.5, height};
        }
        maximum = std::max(maximum, x + advance);
        if (i == text.size()) {
            break;
        }
        if (newline && node.kind == Kind::TextArea) {
            x = 0;
            y += height;
        } else {
            x += advance;
        }
        i = next;
    }
    if (editor.reveal) {
        editor.scroll_x =
            std::clamp(editor.scroll_x, caret.x - std::max(width, space) + space, caret.x);
        editor.scroll_y = std::clamp(editor.scroll_y, caret.y - view_height + height, caret.y);
        editor.reveal = false;
    }
    editor.scroll_x = std::clamp(editor.scroll_x, 0.0, std::max(0.0, maximum - width));
    editor.scroll_y = std::clamp(editor.scroll_y, 0.0, std::max(0.0, y + height - view_height));
    item.shaped = {};
    node.text_cells.clear();
    const auto start = std::min(editor.Cursor(), editor.Anchor());
    const auto end = std::max(editor.Cursor(), editor.Anchor());
    for (std::size_t j = 0; j < cells.size(); ++j) {
        auto cell = cells[j];
        cell.x += padding - editor.scroll_x;
        cell.y += padding - editor.scroll_y;
        if (cell.y + height < 0 || cell.y > node.bounds.height) {
            continue;
        }
        node.text_cells.emplace_back(offsets[j], cell);
        if (offsets[j] >= start && offsets[j] < end) {
            item.text_selection.push_back(cell);
        }
        if (cell.x + cell.width < 0 || cell.x > node.bounds.width || offsets[j] == text.size()) {
            continue;
        }
        const auto scalar = std::string_view(text).substr(
            offsets[j], TextBuffer::Next(text, offsets[j]) - offsets[j]);
        if (scalar == "\n" || scalar == "\r\n" || scalar == "\r" || scalar == "\t") {
            continue;
        }
        const auto &shaped = shapes.at(std::string(scalar));
        for (auto glyph : shaped.glyphs) {
            glyph.origin.x += cell.x;
            glyph.origin.y += cell.y;
            item.shaped.glyphs.push_back(glyph);
        }
    }
    caret.x += padding - editor.scroll_x;
    caret.y += padding - editor.scroll_y;
    item.text_caret = caret;
}
} // namespace prism::runtime
