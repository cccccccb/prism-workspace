#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/theme/compiler.hpp"
#include "prism/theme/motion_compiler.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <vector>

namespace {
using namespace prism;

struct Action {
    contracts::NodeId node;
    std::string name;
};

void Collect(const runtime::Blueprint &blueprint, std::uint32_t &index,
             std::vector<Action> &actions)
{
    const auto own = index++;
    for (const auto &property : blueprint.properties) {
        if (property.id == runtime::DslProperty::Action) {
            actions.push_back({{own, 1}, std::get<std::string>(property.value)});
        }
    }
    for (const auto &child : blueprint.children) {
        Collect(child, index, actions);
    }
}
} // namespace

int main()
{
    const std::filesystem::path root(PRISM_SOURCE_ROOT);
    std::ifstream input(root / "docs/skills/prism-app-ui/assets/examples/compact-library.prism");
    assert(input);
    const std::string source((std::istreambuf_iterator<char>(input)), {});
    const auto blueprint = runtime::ParseBlueprint(source);
    std::uint32_t index{};
    std::vector<Action> actions;
    Collect(blueprint, index, actions);
    assert(actions.size() == 2);
    runtime::TextShaper shaper("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    assert(shaper.Ready());
    const auto shape = std::bind_front(
        static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
            &runtime::TextShaper::Shape),
        &shaper);

    for (const auto *material : {"glass", "translucent", "transparent", "square"}) {
        for (const auto *palette : {"light", "dark"}) {
            for (const auto *motion : {"prism", "subtle", "instant"}) {
                auto theme = theme::LoadTheme(root / "resources/themes", material, 1, palette);
                theme.motion = theme::LoadMotion(root / "resources/motions", motion);
                for (const auto size : {contracts::LogicalSize{244, 420}, {482, 204}, {482, 420}}) {
                    runtime::Scene scene(blueprint, shape, shaper.FontId(), theme);
                    assert(scene.SetViewport(size));
                    assert(scene.Build({1}));
                    for (const auto &action : actions) {
                        const auto rect = scene.Bounds(action.node);
                        assert(rect.width == 32 && rect.height == 32);
                        assert(rect.x >= 0 && rect.y >= 0 && rect.x + rect.width <= size.width &&
                               rect.y + rect.height <= size.height);
                        assert(scene.ActionAt({rect.x + 16, rect.y + 16}) == action.name);
                    }
                }
            }
        }
    }
}
