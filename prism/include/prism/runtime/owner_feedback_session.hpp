#pragma once

#include <cstdint>
#include <optional>

namespace prism::runtime {
// Owner-thread, one-shot visible-time deadline. Rendering does not advance it.
// A zero duration is persistent; a generation begins timing only after adoption.
class OwnerFeedbackSession {
public:
    void Begin(std::uint64_t generation, std::uint64_t duration_ns);
    void Clear() noexcept;
    bool Adopt(std::uint64_t generation, std::uint64_t now_ns, bool paused = false) noexcept;
    void Pause(std::uint64_t now_ns) noexcept;
    void Resume(std::uint64_t now_ns) noexcept;
    std::optional<std::uint64_t> Deadline() const noexcept;
    bool Expired(std::uint64_t now_ns) const noexcept;
    bool Adopted() const noexcept;
    std::uint64_t Generation() const noexcept;

private:
    std::uint64_t generation_{}, remaining_ns_{};
    bool adopted_{}, paused_{}, persistent_{};
    std::optional<std::uint64_t> deadline_;
};
} // namespace prism::runtime
