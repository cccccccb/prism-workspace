#include "scene_p.hpp"
#include <limits>

namespace prism::runtime {
bool Scene::HandleTextInput(const contracts::WindowEvent &event, InteractionResult &result,
                            const std::shared_ptr<const InputSnapshot> &snapshot, bool submitted)
{
    Node *node = nullptr;
    const auto *key = std::get_if<contracts::KeyEvent>(&event);
    const auto *text = std::get_if<contracts::TextInputEvent>(&event);
    const auto *button = std::get_if<contracts::PointerButtonEvent>(&event);
    const auto *motion = std::get_if<contracts::PointerMotionEvent>(&event);
    const auto *scroll = std::get_if<contracts::PointerScrollEvent>(&event);
    if (button || scroll) {
        const auto hit =
            InputHit(button ? button->position : scroll->position, snapshot, submitted);
        node = hit ? Find(hit->node) : nullptr;
    } else if (motion) {
        for (const auto &pointer : input_state_->pointers) {
            if (pointer.source == motion->source && pointer.captured) {
                node = Find(pointer.captured);
            }
        }
    } else if (key || text) {
        for (const auto &focus : input_state_->focus) {
            if (!key || focus.seat == key->source.seat) {
                node = Find(focus.node);
                break;
            }
        }
    }
    if (!node || (node->kind != Kind::TextArea && node->kind != Kind::TextField) ||
        !IsInteractive(node->id) || (submitted && !IsInteractive(node->id, snapshot.get()))) {
        return false;
    }
    auto &editor = node->editor;
    const auto before = editor.Text();
    if (button || motion) {
        if (button && button->button != contracts::PointerButton::Primary) {
            return true;
        }
        if (button) {
            HandleInputButton(*button, snapshot, submitted);
        }
        if (button && button->state == contracts::ButtonState::Released) {
            return true;
        }
        const auto point = button ? button->position : motion->position;
        const double x = point.x - node->bounds.x, y = point.y - node->bounds.y;
        double best = std::numeric_limits<double>::max();
        std::size_t offset = editor.Cursor();
        for (const auto &[index, cell] : node->text_cells) {
            const double dy = y < cell.y                 ? cell.y - y
                              : y > cell.y + cell.height ? y - cell.y - cell.height
                                                         : 0;
            const double distance = dy * 100000 + std::abs(x - cell.x);
            if (distance < best) {
                best = distance;
                offset = index;
            }
        }
        editor.Select(offset, motion != nullptr);
    } else if (scroll) {
        if (!std::isfinite(scroll->delta_x) || !std::isfinite(scroll->delta_y)) {
            return true;
        }
        editor.scroll_y = std::max(0.0, editor.scroll_y + scroll->delta_y * 3);
        editor.scroll_x = std::max(0.0, editor.scroll_x + scroll->delta_x * 3);
        editor.reveal = false;
    } else if (text) {
        if (node->kind == Kind::TextField &&
            text->utf8.find_first_of("\r\n") != std::string::npos) {
            return true;
        }
        editor.Insert(text->utf8);
    } else if (key) {
        if (key->physical_key == 0x2b || key->physical_key == 0x29) {
            return false;
        }
        if (key->state != contracts::ButtonState::Pressed) {
            return true;
        }
        if (key->modifiers.alt || key->modifiers.meta) {
            return true;
        }
        if (key->modifiers.control && (key->physical_key == 0x06 || key->physical_key == 0x1b)) {
            input_state_->clipboard = editor.Selection();
            if (key->physical_key == 0x1b) {
                editor.Insert("");
            }
        } else if (key->modifiers.control && key->physical_key == 0x19) {
            if (node->kind == Kind::TextArea ||
                input_state_->clipboard.find_first_of("\r\n") == std::string::npos) {
                editor.Insert(input_state_->clipboard);
            }
        } else {
            editor.Key(key->physical_key, key->modifiers.control, key->modifiers.shift,
                       node->kind == Kind::TextArea);
        }
    }

    if (editor.Text() != before) {
        node->text = editor.Text();
        node->properties[DslProperty::Text] = node->text;
        ++transaction_revision_;
        result.text_edit = TextEdit{node->action, node->text};
    }
    ++node->revision;
    Invalidate(Dirty::Paint);
    result.changed = true;
    return true;
}
} // namespace prism::runtime
