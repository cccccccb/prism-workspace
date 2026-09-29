#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/load_plan.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/sdk/module_session.hpp"
#include "prism/theme/compiler.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <poll.h>
#include <string>
#include <variant>
#include <vector>

namespace {
struct TemplateClock final : prism::animation::AnimationClock {
    std::uint64_t now{1'000'000'000};

    std::uint64_t NowNs() const noexcept override
    {
        return now;
    }
};

std::string ReadSource(const std::filesystem::path &path)
{
    std::ifstream file(path);
    assert(file);
    return {std::istreambuf_iterator<char>{file}, {}};
}

prism::contracts::ResourceId ResolveImage(std::string_view)
{
    return {12};
}

prism::runtime::ShapedText Shape(std::string_view text, double font)
{
    return {{}, text.size() * font * 0.5, font};
}

struct TemplateContent {
    prism::runtime::Blueprint blueprint;
    std::vector<prism::runtime::LoadBinding> declarations;
};

TemplateContent ReadTemplate(const std::filesystem::path &path)
{
    const auto source = ReadSource(path);
    if (!prism::runtime::IsInterfaceSource(source)) {
        return {prism::runtime::ParseBlueprint(source, ResolveImage), {}};
    }
    const auto plan = prism::runtime::CompileLoadPlan(source, {"master", path.string(), "test"},
                                                      path.parent_path());
    const auto layout =
        prism::runtime::PrepareLayout(ReadSource(plan.package_root / plan.layout_path), plan);
    std::vector<prism::runtime::PreparedUnit> units;
    std::vector<prism::runtime::RegionUpdate> deferred;
    for (const auto &unit : plan.components) {
        auto prepared =
            prism::runtime::PrepareComponent(ReadSource(plan.package_root / unit.source_path),
                                             {unit.id, unit.source_path.string(), "test"});
        if (unit.phase == prism::runtime::LoadPhase::Deferred) {
            deferred.push_back({unit.id, prism::runtime::LinkComponent(prepared, ResolveImage)});
        }
        units.push_back({unit.id, std::move(prepared)});
    }
    const auto critical = prism::runtime::ComposeCritical(plan, layout, units);
    auto blueprint = prism::runtime::LinkComponent(critical, ResolveImage);
    if (!deferred.empty()) {
        // This test validates the completed template layout. Runtime deferred
        // readiness and mount ordering are tested separately by native probes.
        prism::runtime::Scene candidate(
            std::move(blueprint), Shape, prism::contracts::ResourceId{1},
            prism::theme::LoadTheme(prism::theme::DefaultThemeRoot(), "glass"));
        blueprint = candidate.RegionBlueprint(deferred);
    }
    return {std::move(blueprint), plan.bindings};
}

class TemplateBindings {
public:
    TemplateBindings(prism::runtime::Scene &scene,
                     const std::vector<prism::runtime::LoadBinding> &declarations)
        : scene_(scene)
    {
        for (const auto &binding : declarations) {
            types_.emplace(binding.name, binding.type);
            assert(Set(binding.name, binding.initial));
        }
    }

    bool Set(std::string_view key, prism::runtime::PropertyValue value)
    {
        const auto declared = types_.find(key);
        if (declared != types_.end()) {
            using Type = prism::runtime::LoadBindingType;
            const bool valid =
                (declared->second == Type::String && std::holds_alternative<std::string>(value)) ||
                (declared->second == Type::Number && std::holds_alternative<double>(value)) ||
                (declared->second == Type::Boolean && std::holds_alternative<bool>(value)) ||
                (declared->second == Type::Color &&
                 std::holds_alternative<prism::contracts::Color>(value));
            if (!valid) {
                return false;
            }
            if (scene_.AcceptsBinding(key, value)) {
                scene_.SetBinding(key, value);
            }
            return true;
        }
        if (!scene_.AcceptsBinding(key, value)) {
            return false;
        }
        scene_.SetBinding(key, std::move(value));
        return true;
    }

private:
    prism::runtime::Scene &scene_;
    std::map<std::string, prism::runtime::LoadBindingType, std::less<>> types_;
};

struct ModuleRequests {
    std::uint64_t theme_request{7};

    std::uint64_t Launch(std::string_view)
    {
        return 6;
    }

    std::uint64_t Subscribe()
    {
        return 5;
    }

    std::uint64_t Theme(std::string_view)
    {
        return theme_request++;
    }
};

void AwaitReady(prism::sdk::ModuleSession &module)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!module.BackendReady()) {
        assert(module.WorkPending() && std::chrono::steady_clock::now() < deadline);
        pollfd source{module.WorkCompletionFd(), POLLIN, 0};
        assert(poll(&source, 1, 5000) == 1 && (source.revents & POLLIN));
        assert(module.DispatchWork() > 0);
    }
}
} // namespace

struct ActionNode {
    prism::contracts::NodeId node;
    std::string action;
};

void CollectActions(const prism::runtime::Blueprint &blueprint, std::uint64_t &id,
                    std::vector<ActionNode> &actions, std::vector<ActionNode> &controls)
{
    const auto own = id++;
    std::string action;
    for (const auto &property : blueprint.properties) {
        if (property.id == prism::runtime::DslProperty::Action) {
            action = std::get<std::string>(property.value);
            actions.push_back(
                {prism::contracts::NodeId{static_cast<std::uint32_t>(own), 1}, action});
        }
    }
    if (blueprint.kind == prism::runtime::Kind::InteractionTarget) {
        controls.push_back({prism::contracts::NodeId{static_cast<std::uint32_t>(own), 1}, action});
    }
    for (const auto &child : blueprint.children) {
        CollectActions(child, id, actions, controls);
    }
}

void SettleShellMotion(prism::runtime::Scene &scene, TemplateClock &clock)
{
    scene.Build(prism::contracts::WindowId{1});
    assert(scene.HasActiveAnimations());
    clock.now += 200'000'000;
    assert(scene.AdvanceAnimations(clock.now));
    assert(scene.Build(prism::contracts::WindowId{1}));
    assert(!scene.HasActiveAnimations());
}

void CheckShellControls(prism::runtime::Scene &scene, const std::vector<ActionNode> &controls,
                        TemplateClock &clock)
{
    using namespace prism::contracts;
    constexpr WindowId window{1};
    constexpr InputSource pointer{1, 17, 1};
    for (const auto &target : controls) {
        if (!scene.IsVisible(target.node)) {
            continue;
        }
        const auto bounds = scene.Bounds(target.node);
        const auto regions = scene.InputRegions();
        const auto layouts = scene.GetRenderStats().layouts;
        const LogicalPoint position{bounds.x + bounds.width / 2, bounds.y + bounds.height - 1};
        const auto hit = scene.HitTest(position);
        assert(hit && hit->node == target.node);

        assert(scene.HandleInput(PointerMotionEvent{window, position, clock.now, pointer}).changed);
        assert(scene.State(target.node).hovered);
        SettleShellMotion(scene, clock);
        const auto down = scene.HandleInput(PointerButtonEvent{
            window, position, PointerButton::Primary, ButtonState::Pressed, 0, clock.now, pointer});
        assert(down.changed && !down.activation);
        SettleShellMotion(scene, clock);

        const auto up =
            scene.HandleInput(PointerButtonEvent{window, position, PointerButton::Primary,
                                                 ButtonState::Released, 0, clock.now, pointer});
        assert(up.changed);
        if (target.action.empty()) {
            assert(!up.activation);
        } else {
            assert(up.activation && up.activation->node == target.node &&
                   up.activation->action == target.action);
        }
        SettleShellMotion(scene, clock);
        assert(scene.HandleInput(PointerLeaveEvent{window, clock.now, pointer}).changed);
        SettleShellMotion(scene, clock);

        assert(scene.Bounds(target.node) == bounds);
        assert(scene.InputRegions() == regions);
        assert(scene.GetRenderStats().layouts == layouts);
        assert(!scene.Build(window));
    }
}

void CheckActions(prism::runtime::Scene &scene, const std::vector<ActionNode> &actions,
                  double width, double height)
{
    for (const auto &target : actions) {
        if (!scene.IsVisible(target.node)) {
            const auto hidden = scene.Bounds(target.node);
            assert(hidden.width == 0 && hidden.height == 0);
            continue;
        }
        const auto bounds = scene.Bounds(target.node);
        assert(bounds.width > 0 && bounds.height > 0);
        assert(bounds.x >= 0 && bounds.y >= 0 && bounds.x + bounds.width <= width + 0.001 &&
               bounds.y + bounds.height <= height + 0.001);
        assert(scene.ActionAt({bounds.x + bounds.width / 2, bounds.y + bounds.height / 2}) ==
               target.action);
    }
}

void SubmitTheme(prism::runtime::Scene &scene)
{
    const auto dirty = scene.PendingDirty();
    const bool pixels = prism::runtime::Has(dirty, prism::runtime::Dirty::Layout) ||
                        prism::runtime::Has(dirty, prism::runtime::Dirty::Paint);
    const auto list = scene.Build(prism::contracts::WindowId{1});
    assert(list.has_value() == pixels);
    // A template with no reference to changed theme values stays pixel-clean.
    scene.AcknowledgeComposite();
    assert(scene.PendingDirty() == prism::runtime::Dirty::None);
}

int main(int argc, char **argv)
{
    struct Template {
        const char *path;
        double width, height;
    };

    const Template cases[]{{"prism-desktop/ui/desktop.prism", 1024, 600},
                           {"prism-topbar/ui/topbar.prism", 1024, 52},
                           {"prism-dock/ui/dock.prism", 620, 100},
                           {"demos/demo_player/master.prism", 482, 420},
                           {"demos/demo_settings/master.prism", 482, 420},
                           {"demos/demo_player/preview.prism", 482, 420}};
    for (unsigned n = 0; n < std::size(cases); ++n) {
        const auto &item = cases[n];
        auto content = ReadTemplate(std::filesystem::path(PRISM_SOURCE_ROOT) / item.path);
        std::uint64_t id{};
        std::vector<ActionNode> actions;
        std::vector<ActionNode> controls;
        CollectActions(content.blueprint, id, actions, controls);
        assert(controls.size() == (n == 1 ? 1 : n == 2 ? 2 : 0));
        TemplateClock clock;
        prism::runtime::Scene scene(
            std::move(content.blueprint), Shape, prism::contracts::ResourceId{1},
            prism::theme::LoadTheme(prism::theme::DefaultThemeRoot(), "glass"));
        if (!controls.empty()) {
            scene.EnableAnimations(&clock);
        }
        TemplateBindings bindings(scene, content.declarations);
        assert(scene.SetViewport({item.width, item.height}));
        if (argc == 7 && n < 5) {
            // Business startup must satisfy the actual template binding schema,
            // including typed progress, color and icon properties.
            ModuleRequests requests;
            auto &theme_request = requests.theme_request;
            prism::sdk::ModuleSession module(
                argv[n + 1], "template_test", 42,
                std::bind_front(&TemplateBindings::Set, &bindings),
                std::bind_front(&ModuleRequests::Launch, &requests),
                std::bind_front(&ModuleRequests::Subscribe, &requests),
                std::bind_front(&ModuleRequests::Theme, &requests),
                std::bind_front(&ModuleRequests::Theme, &requests), {}, {},
                n == 3 ? std::filesystem::path(argv[6]) : std::filesystem::path{});
            assert(module.Start());
            AwaitReady(module);
            module.Action(n == 3 ? "player:toggle" : n == 4 ? "theme:transparent" : "ignored");
            if (n == 4) {
                module.Deliver(prism::contracts::ThemeEvent{
                    0, 0, prism::contracts::ThemeStatus::Current, "glass", "Prism Glass"});
            }
            module.Tick(prism::sdk::MonotonicNs() + 1000000000ULL);
            if (n == 3) {
                for (const auto page : {"nav:library", "nav:favorites"}) {
                    module.Action(page);
                    for (const auto size : {prism::contracts::LogicalSize{482, 420},
                                            prism::contracts::LogicalSize{482, 204},
                                            prism::contracts::LogicalSize{244, 420}}) {
                        assert(scene.SetViewport(size));
                        SubmitTheme(scene);
                        CheckActions(scene, actions, size.width, size.height);
                    }
                }
                assert(scene.SetViewport({item.width, item.height}));
            }
            if (n == 4) {
                // Both settings pages must fit every real BSP allocation, in
                // every material/palette combination. Hidden actions must not hit.
                for (const auto page : {"page:performance", "page:appearance"}) {
                    module.Action(page);
                    for (const auto size : {prism::contracts::LogicalSize{482, 420},
                                            prism::contracts::LogicalSize{482, 204},
                                            prism::contracts::LogicalSize{244, 420}}) {
                        assert(scene.SetViewport(size));
                        for (const auto *theme :
                             {"glass", "translucent", "transparent", "square"}) {
                            for (const auto *scheme : {"dark", "light"}) {
                                assert(scene.ApplyTheme(
                                    prism::theme::LoadTheme(prism::theme::DefaultThemeRoot(), theme,
                                                            theme_request++, scheme)));
                                SubmitTheme(scene);
                                CheckActions(scene, actions, size.width, size.height);
                            }
                        }
                    }
                }
                module.Action("page:performance");
                assert(scene.SetViewport({item.width, item.height}));
            }
            if (n == 2) {
                using Change = prism::contracts::InstanceChange;
                const auto check = [&] {
                    assert(scene.Build(prism::contracts::WindowId{1}));
                    CheckActions(scene, actions, item.width, item.height);
                    CheckShellControls(scene, controls, clock);
                };
                // Zero, one and two running app groups must all have clickable
                // visible entries; hiding a group must not leave stale geometry.
                check();
                module.Deliver(
                    prism::contracts::InstanceUpdate{{5}, {8}, 42, Change::Running, "demo_player"});
                check();
                module.Deliver(prism::contracts::InstanceUpdate{
                    {5}, {9}, 43, Change::Running, "demo_settings"});
                check();
                module.Deliver(
                    prism::contracts::InstanceUpdate{{5}, {8}, 42, Change::Stopped, "demo_player"});
                check();
                module.Deliver(prism::contracts::InstanceUpdate{
                    {5}, {9}, 43, Change::Stopped, "demo_settings"});
                check();
                module.Deliver(
                    prism::contracts::InstanceUpdate{{5}, {8}, 42, Change::Running, "demo_player"});
            }
        }
        assert(scene.Build(prism::contracts::WindowId{1}));
        CheckActions(scene, actions, item.width, item.height);
        CheckShellControls(scene, controls, clock);
        std::uint64_t generation = 1;
        for (const auto *theme : {"translucent", "transparent", "square", "glass"}) {
            assert(scene.ApplyTheme(
                prism::theme::LoadTheme(prism::theme::DefaultThemeRoot(), theme, generation++)));
            SubmitTheme(scene);
            CheckActions(scene, actions, item.width, item.height);
            CheckShellControls(scene, controls, clock);
            assert(!scene.InputRegions().empty());
        }
        if (n == 3 || n == 4) {
            for (const auto size : {prism::contracts::LogicalSize{482, 204},
                                    prism::contracts::LogicalSize{244, 420}}) {
                assert(scene.SetViewport(size));
                assert(scene.Build(prism::contracts::WindowId{1}));
                CheckActions(scene, actions, size.width, size.height);
            }
        }
    }
}
