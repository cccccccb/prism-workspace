#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace prism::contracts {
inline constexpr std::uint32_t kOwnerFeedbackCapability = 1u << 0;
inline constexpr std::size_t kMaxOwnerFeedbackTitleBytes = 256;
inline constexpr std::size_t kMaxOwnerFeedbackMessageBytes = 2048;
inline constexpr std::size_t kMaxOwnerFeedbackActionLabelBytes = 48;
inline constexpr std::size_t kMaxOwnerFeedbackActions = 2;
inline constexpr std::uint32_t kDefaultOwnerFeedbackDurationMs = 4000;

enum class OwnerFeedbackKind : std::uint32_t { Info = 0, Success = 1, Error = 2 };

struct OwnerFeedbackChoice {
    std::uint32_t id{};
    std::string label;
    bool operator==(const OwnerFeedbackChoice &) const = default;
};

struct OwnerFeedbackRequest {
    std::uint64_t request_id{};
    OwnerFeedbackKind kind{OwnerFeedbackKind::Info};
    std::string title;
    std::string message;
    std::vector<OwnerFeedbackChoice> actions;
    std::uint32_t duration_ms{};
    bool operator==(const OwnerFeedbackRequest &) const = default;
};

struct OwnerFeedbackAction {
    std::uint64_t request_id{};
    std::uint32_t action_id{};
    bool operator==(const OwnerFeedbackAction &) const = default;
};

// Pure bounded value validation. No filesystem, Scene, Host or WM authority.
bool ValidateOwnerFeedbackRequest(const OwnerFeedbackRequest &) noexcept;
bool ValidateOwnerFeedbackAction(const OwnerFeedbackAction &,
                                 const OwnerFeedbackRequest &) noexcept;
} // namespace prism::contracts
