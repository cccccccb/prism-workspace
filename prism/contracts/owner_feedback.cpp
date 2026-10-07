#include "prism/contracts/owner_feedback.hpp"

#include <string_view>

namespace prism::contracts {
namespace {
bool Allowed(std::uint32_t point, bool multiline) noexcept
{
    return (multiline && point == '\n') || (point >= 0x20 && !(point >= 0x7f && point <= 0x9f) &&
                                            point != 0x2028 && point != 0x2029);
}

bool ValidText(std::string_view text, std::size_t maximum, bool multiline) noexcept
{
    if (text.size() > maximum) {
        return false;
    }

    for (std::size_t at = 0; at < text.size();) {
        auto byte = static_cast<unsigned char>(text[at++]);
        std::uint32_t point = byte;
        std::uint32_t minimum{};
        unsigned remaining{};
        if (byte < 0x80) {
            if (!Allowed(point, multiline)) {
                return false;
            }
            continue;
        }
        if ((byte & 0xe0) == 0xc0) {
            remaining = 1;
            point = byte & 0x1f;
            minimum = 0x80;
        } else if ((byte & 0xf0) == 0xe0) {
            remaining = 2;
            point = byte & 0x0f;
            minimum = 0x800;
        } else if ((byte & 0xf8) == 0xf0) {
            remaining = 3;
            point = byte & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (remaining > text.size() - at) {
            return false;
        }
        while (remaining--) {
            byte = static_cast<unsigned char>(text[at++]);
            if ((byte & 0xc0) != 0x80) {
                return false;
            }
            point = (point << 6) | (byte & 0x3f);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) ||
            !Allowed(point, multiline)) {
            return false;
        }
    }
    return true;
}
} // namespace

bool ValidateOwnerFeedbackRequest(const OwnerFeedbackRequest &request) noexcept
{
    if (!request.request_id || request.title.empty() ||
        !ValidText(request.title, kMaxOwnerFeedbackTitleBytes, false) ||
        !ValidText(request.message, kMaxOwnerFeedbackMessageBytes, true) ||
        request.actions.size() > kMaxOwnerFeedbackActions) {
        return false;
    }
    switch (request.kind) {
    case OwnerFeedbackKind::Info:
    case OwnerFeedbackKind::Success:
        if (request.duration_ms && (request.duration_ms < 1000 || request.duration_ms > 30000)) {
            return false;
        }
        break;
    case OwnerFeedbackKind::Error:
        if (request.duration_ms) {
            return false;
        }
        break;
    default:
        return false;
    }

    for (std::size_t at = 0; at < request.actions.size(); ++at) {
        const auto &action = request.actions[at];
        if (!action.id || action.label.empty() ||
            !ValidText(action.label, kMaxOwnerFeedbackActionLabelBytes, false)) {
            return false;
        }
        for (std::size_t previous = 0; previous < at; ++previous) {
            if (request.actions[previous].id == action.id) {
                return false;
            }
        }
    }
    return true;
}

bool ValidateOwnerFeedbackAction(const OwnerFeedbackAction &action,
                                 const OwnerFeedbackRequest &request) noexcept
{
    if (!ValidateOwnerFeedbackRequest(request) || action.request_id != request.request_id ||
        !action.action_id) {
        return false;
    }
    for (const auto &choice : request.actions) {
        if (choice.id == action.action_id) {
            return true;
        }
    }
    return false;
}
} // namespace prism::contracts
