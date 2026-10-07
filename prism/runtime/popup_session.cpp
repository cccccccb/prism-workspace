#include "prism/runtime/popup.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace prism::runtime {
PopupSession::PopupSession(std::uint64_t owner) : owner_(owner)
{
    if (!owner_) {
        throw std::invalid_argument("Popup owner lifetime must be nonzero");
    }
}

std::optional<std::uint64_t>
PopupSession::Open(PopupIdentity identity, contracts::NodeId return_focus, std::uint64_t parent)
{
    if (identity.owner != owner_ || !identity.scene || !identity.anchor ||
        next_token_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    std::size_t keep = 0;
    if (parent) {
        const auto found =
            std::find_if(entries_.begin(), entries_.end(),
                         [parent](const PopupEntry &entry) { return entry.token == parent; });
        if (found == entries_.end() || found->identity.scene != identity.scene) {
            return std::nullopt;
        }
        keep = static_cast<std::size_t>(found - entries_.begin()) + 1;
    }
    // Reopening an ancestor as its own descendant is not a submenu.
    for (std::size_t i = 0; i < keep; ++i) {
        if (entries_[i].identity == identity) {
            return std::nullopt;
        }
    }

    entries_.reserve(std::max(entries_.size(), keep + 1));
    CloseFrom(keep, PopupCloseReason::Replaced);
    const auto token = next_token_++;
    entries_.push_back({token, identity, return_focus});

    return token;
}

void PopupSession::CloseFrom(std::size_t index, PopupCloseReason reason)
{
    closures_.reserve(closures_.size() + entries_.size() - index);
    while (entries_.size() > index) {
        closures_.push_back({entries_.back(), reason});
        entries_.pop_back();
    }
}

bool PopupSession::Close(std::uint64_t token, PopupCloseReason reason)
{
    const auto found =
        std::find_if(entries_.begin(), entries_.end(),
                     [token](const PopupEntry &entry) { return entry.token == token; });
    if (found == entries_.end()) {
        return false;
    }

    CloseFrom(static_cast<std::size_t>(found - entries_.begin()), reason);
    return true;
}

bool PopupSession::Escape()
{
    return !entries_.empty() && Close(entries_.back().token, PopupCloseReason::Escape);
}

bool PopupSession::Command(std::uint64_t token)
{
    const auto found =
        std::find_if(entries_.begin(), entries_.end(),
                     [token](const PopupEntry &entry) { return entry.token == token; });
    if (found == entries_.end()) {
        return false;
    }

    CloseAll(PopupCloseReason::Command);
    return true;
}

bool PopupSession::Invalidate(PopupIdentity identity)
{
    const auto found =
        std::find_if(entries_.begin(), entries_.end(),
                     [identity](const PopupEntry &entry) { return entry.identity == identity; });
    if (found == entries_.end()) {
        return false;
    }

    CloseFrom(static_cast<std::size_t>(found - entries_.begin()), PopupCloseReason::Unavailable);
    return true;
}

void PopupSession::CloseAll(PopupCloseReason reason)
{
    CloseFrom(0, reason);
}

const std::vector<PopupEntry> &PopupSession::Entries() const noexcept
{
    return entries_;
}

std::vector<PopupClosure> PopupSession::TakeClosures()
{
    return std::exchange(closures_, {});
}
} // namespace prism::runtime
