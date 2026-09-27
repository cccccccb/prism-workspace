#pragma once

#include <cstdint>

namespace prism::platform {
// Local to one window object; zero never identifies a successful pixel commit.
struct PixelSubmissionId {
    std::uint64_t value{};

    explicit operator bool() const noexcept
    {
        return value != 0;
    }

    bool operator==(const PixelSubmissionId &) const noexcept = default;
};

enum class PresentationOutcome { Presented, Discarded };

struct PixelPresentation {
    PixelSubmissionId submission;
    PresentationOutcome outcome{PresentationOutcome::Discarded};
};
} // namespace prism::platform
