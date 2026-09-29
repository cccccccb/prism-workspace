#include "scene_p.hpp"
#include <stdexcept>

namespace prism::runtime {
bool Scene::ApplyTheme(const contracts::ThemeSnapshot &theme, std::string *diagnostic)
{
    try {
        if (!root_) {
            throw std::invalid_argument("Scene candidate has been consumed");
        }
        contracts::ValidateTheme(theme);
        if (theme_ && *theme_ == theme) {
            if (diagnostic) {
                diagnostic->clear();
            }
            return true;
        }
        Scene candidate(CurrentBlueprint(*root_), shaper_, font_, theme);
        CopyResources(*root_, *candidate.root_);
        ValidateCandidate(candidate, {});
        std::vector<std::pair<Node *, Node *>> pairs;
        CollectPairs(*root_, *candidate.root_, pairs);
        Dirty affected = Dirty::None;
        const bool controls_changed = !theme_ || theme_->controls != theme.controls;
        for (const auto &[live, prepared] : pairs) {
            if (!IsVisible(*live)) {
                continue;
            }
            for (unsigned id = 0; id <= static_cast<unsigned>(DslProperty::Visible); ++id) {
                const auto property = static_cast<DslProperty>(id);
                if (property != DslProperty::Material &&
                    CurrentProperty(*live, property) !=
                        candidate.CurrentProperty(*prepared, property)) {
                    affected = affected | FindProperty(property)->affects;
                }
            }
        }
        if (controls_changed) {
            affected = affected | Dirty::Paint;
        }

        CancelAnimations();
        theme_.swap(candidate.theme_);
        CommitValues(pairs);
        ReconcileInput();
        ++transaction_revision_;
        Invalidate(affected);
        hit_geometry_dirty_ = true;
        input_dirty_ = true;
        if (diagnostic) {
            diagnostic->clear();
        }
        return true;
    } catch (const std::exception &error) {
        if (diagnostic) {
            *diagnostic = error.what();
        }
        return false;
    }
}
} // namespace prism::runtime
