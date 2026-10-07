#pragma once

#include "prism/contracts/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace prism::runtime {

enum class PopupSide { Below, Above, EdgePanel };
enum class PopupHorizontalAlignment { Start, Center };

struct PopupPlacementRequest {
    contracts::LogicalRect available;
    contracts::LogicalRect anchor;
    contracts::LogicalSize desired;
    double minimum_width{160};
    double minimum_height{80};
    double margin{8};
    double gap{8};
    PopupHorizontalAlignment horizontal_alignment{PopupHorizontalAlignment::Start};
};

struct PopupPlacement {
    contracts::LogicalRect bounds;
    PopupSide side{PopupSide::Below};
    bool width_constrained{false};
    bool height_constrained{false};
    contracts::LogicalRect effective_anchor;
};

// All rectangles use one owner-provided logical coordinate space. No output lookup occurs here.
std::optional<PopupPlacement> PlacePopup(const PopupPlacementRequest &request) noexcept;

struct PopupIdentity {
    std::uint64_t owner{0}; // Host-issued owner lifetime, not a reusable window index.
    std::uint64_t scene{0};
    contracts::NodeId anchor;
    bool operator==(const PopupIdentity &) const noexcept = default;
};

enum class PopupCloseReason { Escape, OutsidePress, Command, Unavailable, Replaced };

struct PopupEntry {
    std::uint64_t token{0};
    PopupIdentity identity;
    contracts::NodeId return_focus;
};

struct PopupClosure {
    PopupEntry entry;
    PopupCloseReason reason;
};

// Owner-thread policy core. Hit testing, event suppression and focus validation remain
// the responsibility of the input adapter. Closed entries are returned deepest first.
class PopupSession {
public:
    explicit PopupSession(std::uint64_t owner);
    std::optional<std::uint64_t> Open(PopupIdentity identity, contracts::NodeId return_focus,
                                      std::uint64_t parent = 0);
    bool Close(std::uint64_t token, PopupCloseReason reason);
    bool Escape();
    bool Command(std::uint64_t token);
    bool Invalidate(PopupIdentity identity);
    void CloseAll(PopupCloseReason reason);
    const std::vector<PopupEntry> &Entries() const noexcept;
    std::vector<PopupClosure> TakeClosures();

private:
    void CloseFrom(std::size_t index, PopupCloseReason reason);
    std::uint64_t owner_;
    std::uint64_t next_token_{1};
    std::vector<PopupEntry> entries_;
    std::vector<PopupClosure> closures_;
};

} // namespace prism::runtime
