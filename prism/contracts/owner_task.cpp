#include "prism/contracts/owner_task.hpp"

#include <string_view>

namespace prism::contracts {
namespace {
bool AllowedCodePoint(std::uint32_t point, bool multiline) noexcept
{
    if (multiline && (point == '\n' || point == '\t')) {
        return true;
    }
    return point >= 0x20 && !(point >= 0x7f && point <= 0x9f) && point != 0x2028 && point != 0x2029;
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
            if (!AllowedCodePoint(point, multiline)) {
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
            !AllowedCodePoint(point, multiline)) {
            return false;
        }
    }
    return true;
}

bool ValidCancelReason(OwnerTaskCancelReason reason) noexcept
{
    switch (reason) {
    case OwnerTaskCancelReason::User:
    case OwnerTaskCancelReason::Escape:
    case OwnerTaskCancelReason::UiReplaced:
    case OwnerTaskCancelReason::ScopeUnavailable:
    case OwnerTaskCancelReason::FrontendFailed:
        return true;
    case OwnerTaskCancelReason::None:
        return false;
    }
    return false;
}

bool ValidFailureCode(OwnerTaskFailureCode code) noexcept
{
    switch (code) {
    case OwnerTaskFailureCode::PreparationFailed:
    case OwnerTaskFailureCode::OperationFailed:
    case OwnerTaskFailureCode::ProviderUnavailable:
        return true;
    case OwnerTaskFailureCode::None:
        return false;
    }
    return false;
}

bool ValidAbsolutePath(std::string_view path, bool normalized) noexcept
{
    if (path.empty() || path.front() != '/' ||
        !ValidText(path, kMaxOwnerFileTaskPathBytes, false) ||
        (normalized && path.size() > 1 && path.back() == '/')) {
        return false;
    }

    for (std::size_t at = 1; at < path.size();) {
        const auto separator = path.find('/', at);
        const auto end = separator == std::string_view::npos ? path.size() : separator;
        const auto component = path.substr(at, end - at);
        if (component.size() > kMaxOwnerFileTaskNameBytes ||
            (normalized && (component.empty() || component == "." || component == ".."))) {
            return false;
        }
        at = end + 1;
    }
    return true;
}

bool ExtensionCharacter(char character) noexcept
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
}

bool ValidExtension(std::string_view extension) noexcept
{
    if (extension.size() < 2 || extension.size() > kMaxOwnerFileTaskExtensionBytes ||
        extension.front() != '.') {
        return false;
    }

    bool segment{};
    for (std::size_t at = 1; at < extension.size(); ++at) {
        const auto character = extension[at];
        if (character == '.') {
            if (!segment) {
                return false;
            }
            segment = false;
        } else if (ExtensionCharacter(character)) {
            segment = true;
        } else {
            return false;
        }
    }
    return segment;
}

bool ValidChoices(const OwnerTaskRequest &request) noexcept
{
    if (request.choices.empty() || request.choices.size() > kMaxOwnerTaskChoices) {
        return false;
    }

    std::size_t text_bytes = request.title.size() + request.message.size();
    unsigned primary{};
    unsigned destructive{};
    for (std::size_t at = 0; at < request.choices.size(); ++at) {
        const auto &choice = request.choices[at];
        if (!choice.id || choice.label.empty() ||
            !ValidText(choice.label, kMaxOwnerTaskChoiceLabelBytes, false) ||
            choice.label.size() > kMaxOwnerTaskTextBytes - text_bytes) {
            return false;
        }
        for (std::size_t prior = 0; prior < at; ++prior) {
            if (request.choices[prior].id == choice.id) {
                return false;
            }
        }
        text_bytes += choice.label.size();
        switch (choice.role) {
        case OwnerTaskChoiceRole::Secondary:
            break;
        case OwnerTaskChoiceRole::Primary:
            ++primary;
            break;
        case OwnerTaskChoiceRole::Destructive:
            ++destructive;
            break;
        default:
            return false;
        }
    }

    return primary <= 1 && destructive <= 1 && primary + destructive > 0;
}
} // namespace

std::uint32_t OwnerTaskCapability(OwnerTaskKind kind) noexcept
{
    switch (kind) {
    case OwnerTaskKind::Confirmation:
        return kOwnerTaskConfirmationCapability;
    case OwnerTaskKind::OpenFile:
        return kOwnerTaskOpenFileCapability;
    case OwnerTaskKind::SaveFile:
        return kOwnerTaskSaveFileCapability;
    case OwnerTaskKind::SelectDirectory:
        return kOwnerTaskSelectDirectoryCapability;
    }
    return 0;
}

bool ValidOwnerFilePath(std::string_view path) noexcept
{
    return ValidAbsolutePath(path, true);
}

bool ValidOwnerFileDirectoryHint(std::string_view path) noexcept
{
    return path.empty() || ValidAbsolutePath(path, false);
}

bool ValidOwnerFileName(std::string_view name) noexcept
{
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos &&
           ValidText(name, kMaxOwnerFileTaskNameBytes, false);
}

bool ValidOwnerFileExtensions(const std::vector<std::string> &extensions) noexcept
{
    if (extensions.size() > kMaxOwnerFileTaskExtensions) {
        return false;
    }

    for (std::size_t at = 0; at < extensions.size(); ++at) {
        if (!ValidExtension(extensions[at])) {
            return false;
        }
        for (std::size_t prior = 0; prior < at; ++prior) {
            if (extensions[prior] == extensions[at]) {
                return false;
            }
        }
    }
    return true;
}

bool OwnerFileExtensionMatches(std::string_view name,
                               const std::vector<std::string> &extensions) noexcept
{
    if (!ValidOwnerFileName(name) || !ValidOwnerFileExtensions(extensions)) {
        return false;
    }
    if (extensions.empty()) {
        return true;
    }
    for (const auto &extension : extensions) {
        if (name.ends_with(extension)) {
            return true;
        }
    }
    return false;
}

bool ValidateOwnerTaskRequest(const OwnerTaskRequest &request) noexcept
{
    if (!request.request_id || !OwnerTaskCapability(request.kind) || request.title.empty() ||
        !ValidText(request.title, kMaxOwnerTaskTitleBytes, false) ||
        !ValidText(request.message, kMaxOwnerTaskMessageBytes, true)) {
        return false;
    }
    if (request.kind == OwnerTaskKind::Confirmation) {
        return !request.file && ValidChoices(request);
    }
    if (!request.choices.empty() || !request.file) {
        return false;
    }

    const auto &file = *request.file;
    if (!ValidOwnerFileDirectoryHint(file.initial_directory) ||
        !ValidOwnerFileExtensions(file.extensions) ||
        (!file.suggested_name.empty() &&
         (request.kind != OwnerTaskKind::SaveFile || !ValidOwnerFileName(file.suggested_name)))) {
        return false;
    }
    return request.kind != OwnerTaskKind::SelectDirectory || file.extensions.empty();
}

bool ValidateOwnerTaskResult(const OwnerTaskResult &result,
                             const OwnerTaskRequest &request) noexcept
{
    if (!ValidateOwnerTaskRequest(request) || result.request_id != request.request_id) {
        return false;
    }

    switch (result.outcome) {
    case OwnerTaskOutcome::Success:
        if (result.cancel_reason != OwnerTaskCancelReason::None ||
            result.failure_code != OwnerTaskFailureCode::None || !result.diagnostic.empty()) {
            return false;
        }
        if (request.kind != OwnerTaskKind::Confirmation) {
            if (result.choice_id || !ValidOwnerFilePath(result.file_path) ||
                (result.overwrite_approved && request.kind != OwnerTaskKind::SaveFile)) {
                return false;
            }
            if (request.kind == OwnerTaskKind::SelectDirectory) {
                return true;
            }
            const auto name =
                std::string_view(result.file_path).substr(result.file_path.find_last_of('/') + 1);
            return OwnerFileExtensionMatches(name, request.file->extensions);
        }
        if (!result.file_path.empty() || result.overwrite_approved) {
            return false;
        }
        for (const auto &choice : request.choices) {
            if (choice.id == result.choice_id) {
                return true;
            }
        }
        return false;
    case OwnerTaskOutcome::Cancelled:
        return !result.choice_id && ValidCancelReason(result.cancel_reason) &&
               result.failure_code == OwnerTaskFailureCode::None && result.diagnostic.empty() &&
               result.file_path.empty() && !result.overwrite_approved;
    case OwnerTaskOutcome::Failed:
        return !result.choice_id && result.cancel_reason == OwnerTaskCancelReason::None &&
               ValidFailureCode(result.failure_code) &&
               ValidText(result.diagnostic, kMaxOwnerTaskDiagnosticBytes, true) &&
               result.file_path.empty() && !result.overwrite_approved;
    }
    return false;
}
} // namespace prism::contracts
