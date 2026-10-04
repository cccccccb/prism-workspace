#include "prism/contracts/motion.hpp"
#include <set>
#include <stdexcept>

namespace prism::contracts {
bool ValidMotionName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const auto c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

void ValidateMotion(const MotionSet &set)
{
    if (!ValidMotionName(set.id) || set.id.find('.') != std::string::npos ||
        set.transitions.empty() || set.transitions.size() > 64) {
        throw std::invalid_argument("Invalid MotionSet identity or capacity");
    }
    std::set<std::string> names;
    for (const auto &entry : set.transitions) {
        if (!ValidMotionName(entry.name) || !names.insert(entry.name).second ||
            entry.duration_ms > 10000 || static_cast<unsigned>(entry.easing) > 3) {
            throw std::invalid_argument("Invalid or duplicate motion transition");
        }
    }
}

const MotionTransition *FindMotion(const MotionSet &set, std::string_view name) noexcept
{
    for (const auto &entry : set.transitions) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}
} // namespace prism::contracts
