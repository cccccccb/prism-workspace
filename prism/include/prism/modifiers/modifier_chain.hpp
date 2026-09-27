#pragma once

#include "prism/modifiers/modifier.hpp"
#include <memory>
#include <vector>

namespace prism::modifiers {

/**
 * @brief Chain Pattern / Collection of visual modifiers
 */
class ModifierChain {
public:
    ModifierChain() = default;

    ModifierChain &Add(std::shared_ptr<VisualModifier> mod)
    {
        modifiers_.push_back(std::move(mod));
        return *this;
    }

    const std::vector<std::shared_ptr<VisualModifier>> &GetAll() const
    {
        return modifiers_;
    }

    template <typename T> std::shared_ptr<T> Find() const
    {
        for (const auto &mod : modifiers_) {
            if (auto casted = std::dynamic_pointer_cast<T>(mod)) {
                return casted;
            }
        }
        return nullptr;
    }

    bool Has(ModifierType type) const
    {
        for (const auto &mod : modifiers_) {
            if (mod->GetType() == type) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::shared_ptr<VisualModifier>> modifiers_;
};

} // namespace prism::modifiers
