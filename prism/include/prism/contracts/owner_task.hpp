#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism::contracts {
inline constexpr std::uint32_t kOwnerTaskConfirmationCapability = 1u << 0;
inline constexpr std::uint32_t kOwnerTaskOpenFileCapability = 1u << 1;
inline constexpr std::uint32_t kOwnerTaskSaveFileCapability = 1u << 2;
inline constexpr std::uint32_t kOwnerTaskSelectDirectoryCapability = 1u << 3;
inline constexpr std::uint32_t kOwnerTaskCapabilities =
    kOwnerTaskConfirmationCapability | kOwnerTaskOpenFileCapability | kOwnerTaskSaveFileCapability |
    kOwnerTaskSelectDirectoryCapability;
inline constexpr std::size_t kMaxOwnerTaskTitleBytes = 256;
inline constexpr std::size_t kMaxOwnerTaskMessageBytes = 2048;
inline constexpr std::size_t kMaxOwnerTaskChoiceLabelBytes = 96;
inline constexpr std::size_t kMaxOwnerTaskTextBytes = 4096;
inline constexpr std::size_t kMaxOwnerTaskDiagnosticBytes = 1024;
inline constexpr std::size_t kMaxOwnerTaskChoices = 2;
inline constexpr std::size_t kMaxOwnerFileTaskPathBytes = 4096;
inline constexpr std::size_t kMaxOwnerFileTaskNameBytes = 255;
inline constexpr std::size_t kMaxOwnerFileTaskExtensions = 16;
inline constexpr std::size_t kMaxOwnerFileTaskExtensionBytes = 32;

enum class OwnerTaskKind : std::uint32_t {
    Confirmation = 1,
    OpenFile = 2,
    SaveFile = 3,
    SelectDirectory = 4
};
enum class OwnerTaskChoiceRole : std::uint32_t { Secondary = 0, Primary = 1, Destructive = 2 };

struct OwnerTaskChoice {
    std::uint32_t id{};
    std::string label;
    OwnerTaskChoiceRole role{OwnerTaskChoiceRole::Secondary};
    bool operator==(const OwnerTaskChoice &) const = default;
};

struct OwnerFileTaskOptions {
    std::string initial_directory;
    std::string suggested_name;
    std::vector<std::string> extensions;
    bool show_hidden{};
    bool operator==(const OwnerFileTaskOptions &) const = default;
};

// Owning provider input. request_id is issued by ModuleSession; Host binds it to
// the actual frontend lifetime. It is not a Scene identity or an IPC credential.
struct OwnerTaskRequest {
    std::uint64_t request_id{};
    OwnerTaskKind kind{OwnerTaskKind::Confirmation};
    std::string title;
    std::string message;
    std::vector<OwnerTaskChoice> choices;
    std::optional<OwnerFileTaskOptions> file;
    bool operator==(const OwnerTaskRequest &) const = default;
};

enum class OwnerTaskOutcome : std::uint32_t { Success = 0, Cancelled = 1, Failed = 2 };
enum class OwnerTaskCancelReason : std::uint32_t {
    None = 0,
    User = 1,
    Escape = 2,
    UiReplaced = 3,
    ScopeUnavailable = 4,
    FrontendFailed = 5
};
enum class OwnerTaskFailureCode : std::uint32_t {
    None = 0,
    PreparationFailed = 1,
    OperationFailed = 2,
    ProviderUnavailable = 3
};

struct OwnerTaskResult {
    std::uint64_t request_id{};
    OwnerTaskOutcome outcome{OwnerTaskOutcome::Success};
    std::uint32_t choice_id{};
    OwnerTaskCancelReason cancel_reason{OwnerTaskCancelReason::None};
    OwnerTaskFailureCode failure_code{OwnerTaskFailureCode::None};
    std::string diagnostic;
    std::string file_path;
    bool overwrite_approved{};
    bool operator==(const OwnerTaskResult &) const = default;
};

// Pure lexical validation only: no filesystem access, Scene or owner authority.
std::uint32_t OwnerTaskCapability(OwnerTaskKind) noexcept;
bool ValidOwnerFilePath(std::string_view) noexcept;
bool ValidOwnerFileDirectoryHint(std::string_view) noexcept;
bool ValidOwnerFileName(std::string_view) noexcept;
bool ValidOwnerFileExtensions(const std::vector<std::string> &) noexcept;
bool OwnerFileExtensionMatches(std::string_view, const std::vector<std::string> &) noexcept;

// Validate without mutation or allocation. Confirmation uses one or two unique
// choices, at most one Primary and one Destructive, and at least one of either.
bool ValidateOwnerTaskRequest(const OwnerTaskRequest &) noexcept;
bool ValidateOwnerTaskResult(const OwnerTaskResult &, const OwnerTaskRequest &) noexcept;
} // namespace prism::contracts
