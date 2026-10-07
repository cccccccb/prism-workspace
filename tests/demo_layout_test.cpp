#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/load_plan.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_shaper.hpp"
#include "prism/theme/compiler.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>

using namespace prism;

namespace {
std::string Read(const std::filesystem::path &path)
{
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}

contracts::ResourceId ResolveImage(std::string_view)
{
    return {12};
}

void Check(runtime::Scene &scene)
{
    assert(scene.Build({1}));
    const auto geometry = scene.InputGeometry();
    for (const auto &node : geometry->nodes) {
        if (node.visible && node.parent) {
            const auto *parent = geometry->Find(node.parent);
            assert(parent);
            const auto b = node.bounds;
            const auto p = parent->bounds;
            // A group must budget for its contents, even when its controls still hit.
            if (b.x < p.x - 0.5 || b.y < p.y - 0.5 || b.x + b.width > p.x + p.width + 0.5 ||
                b.y + b.height > p.y + p.height + 0.5) {
                std::cerr << "Content outside group: node=" << node.id.index
                          << " parent=" << node.parent.index << " action=" << node.action
                          << " bounds=" << b.x << ',' << b.y << ',' << b.width << ',' << b.height
                          << " parent=" << p.x << ',' << p.y << ',' << p.width << ',' << p.height
                          << '\n';
                assert(false);
            }
        }
        if (!node.visible || node.action.empty()) {
            continue;
        }
        const auto b = node.bounds;
        if (b.width < 24 || b.height < 24 || b.x < 0 || b.y < 0 ||
            b.x + b.width > geometry->viewport.width ||
            b.y + b.height > geometry->viewport.height ||
            scene.ActionAt({b.x + b.width / 2, b.y + b.height / 2}) != node.action) {
            std::cerr << "Unreachable control: " << node.action << '\n';
            assert(false);
        }
    }
}

std::shared_ptr<const runtime::InputSnapshot> Present(runtime::Scene &scene)
{
    scene.Build({1});
    auto shown = scene.InputGeometry();
    assert(shown);
    scene.ApplyInputSnapshot(shown);
    return shown;
}

contracts::NodeId FindUniqueAction(const runtime::InputSnapshot &shown, std::string_view action)
{
    contracts::NodeId found;
    for (const auto &node : shown.nodes) {
        if (node.action == action) {
            assert(!found);
            found = node.id;
        }
    }
    assert(found);
    return found;
}

bool InMenu(const runtime::InputSnapshot &shown, contracts::NodeId node, contracts::NodeId menu)
{
    for (std::size_t depth = 0; node && depth < shown.nodes.size(); ++depth) {
        if (node == menu) {
            return true;
        }
        const auto *item = shown.Find(node);
        assert(item);
        node = item->parent;
    }
    return false;
}

contracts::NodeId FindMenuAction(const runtime::InputSnapshot &shown, contracts::NodeId menu,
                                 std::string_view action)
{
    contracts::NodeId found;
    for (const auto &node : shown.nodes) {
        if (node.action == action && InMenu(shown, node.id, menu)) {
            assert(!found && node.visible && node.enabled);
            found = node.id;
        }
    }
    assert(found);
    return found;
}

runtime::InteractionResult Click(runtime::Scene &scene,
                                 const std::shared_ptr<const runtime::InputSnapshot> &shown,
                                 contracts::NodeId target)
{
    const auto *node = shown->Find(target);
    assert(node && node->visible && node->enabled);
    const auto bounds = node->bounds;
    contracts::PointerButtonEvent button{
        {1},
        {bounds.x + bounds.width / 2, bounds.y + bounds.height / 2},
        contracts::PointerButton::Primary,
        contracts::ButtonState::Pressed,
        0,
        1,
        {7, 1, 1}};
    scene.HandleInput(button, shown);
    button.state = contracts::ButtonState::Released;
    return scene.HandleInput(button, shown);
}

void MenuKey(runtime::Scene &scene, const std::shared_ptr<const runtime::InputSnapshot> &shown,
             std::uint32_t key)
{
    scene.HandleInput(
        contracts::KeyEvent{{1}, key, contracts::ButtonState::Pressed, false, 1, {7, 2, 1}, {}},
        shown);
}

void CheckNativePreferencesMenu(runtime::Scene &scene, const runtime::PopupSurfaceRequest &request,
                                contracts::LogicalSize viewport)
{
    const auto parent = request.parent_window_geometry;
    assert(parent == (contracts::LogicalRect{0, 0, viewport.width, viewport.height}));
    const auto width = std::ceil(request.desired_geometry.width);
    const auto height = std::ceil(request.desired_geometry.height);
    const bool above = request.vertical_preference == contracts::PopupVerticalPreference::Above;
    const auto x = std::floor(request.anchor.x + request.anchor.width / 2 - width / 2);
    const auto y = above ? std::floor(request.anchor.y - request.gap - height)
                         : std::ceil(request.anchor.y + request.anchor.height + request.gap);
    const runtime::PopupSurfaceConfigure configure{
        request.parent_configure_generation, 9, {x - parent.x, y - parent.y, width, height}};

    // A native panel may extend outside its parent window. Only normal root
    // content is clipped; an ancestor clip on the Menu must remain an export error.
    std::string diagnostic;
    const auto plan = scene.PreparePopupSurface(request, configure, &diagnostic);
    if (!plan) {
        std::cerr << "Preferences native menu export failed: " << diagnostic << '\n';
        assert(false);
    }
    assert(plan->display_list && plan->input_snapshot);
    contracts::ValidateDisplayList(*plan->display_list);
    assert(plan->request.parent_window_geometry == parent);
    assert(plan->window_geometry.width == width && plan->window_geometry.height == height);
    assert(plan->body_bounds.x == x && plan->body_bounds.width == request.desired_body.width);
    assert(plan->body_bounds.height == request.desired_body.height);
    const auto *panel = plan->input_snapshot->Find(request.active_node);
    assert(panel && panel->contour && panel->parent == contracts::NodeId{});
    assert(contracts::ContourBounds(*panel->contour) == plan->window_geometry);
    const auto tip_y =
        above ? plan->window_geometry.y + plan->window_geometry.height : plan->window_geometry.y;
    std::optional<contracts::LogicalPoint> apex;
    for (const auto point : panel->contour->points) {
        if (point.y == tip_y) {
            assert(!apex); // The actual Settings Menu uses the triangle recipe, not a flat tab.
            apex = point;
        }
    }
    assert(apex);
    assert(std::abs(apex->x - (request.anchor.x + request.anchor.width / 2 -
                               plan->surface_origin.x)) <= 1.0 / 256);
    const auto neck_height = above ? apex->y - plan->body_geometry.y - plan->body_geometry.height
                                   : plan->body_geometry.y - apex->y;
    assert(neck_height > 0);
    if (request.requires_backdrop) {
        assert(plan->effect_regions.size() == 1);
        assert(plan->effect_regions.front().contour == *panel->contour);
        assert(plan->effect_regions.front().bounds == plan->window_geometry);
    }
}

void CheckPreferencesMenu(runtime::Scene &scene, contracts::LogicalSize viewport)
{
    auto shown = Present(scene);
    const auto wide = FindUniqueAction(*shown, "prefs:menu:wide");
    const auto compact = FindUniqueAction(*shown, "prefs:menu:compact");
    const bool wide_visible = viewport.width >= 600;
    assert(shown->Find(wide)->visible == wide_visible);
    assert(shown->Find(compact)->visible != wide_visible);
    const auto trigger = wide_visible ? wide : compact;
    const auto trigger_bounds = shown->Find(trigger)->bounds;
    assert(trigger_bounds.width == 80 && trigger_bounds.height == 32);

    constexpr std::array<std::string_view, 4> commands{"page:performance", "page:appearance",
                                                       "page:system", "sys:refresh"};
    for (std::size_t index = 0; index < commands.size(); ++index) {
        assert(!Click(scene, shown, trigger).activation);
        assert(scene.PopupToken());
        shown = Present(scene);
        const auto request = scene.CapturePopupSurfaceRequest(1);
        assert(request && request->trigger == trigger);
        assert(request->desired_body.width == 280 && request->desired_body.height == 264);
        if (index == 0) {
            CheckNativePreferencesMenu(scene, *request, viewport);
        }
        const auto menu = request->active_node;
        const auto body = scene.Bounds(menu);
        assert(body.width <= 280 && body.height <= 264);
        assert(body.x >= 0 && body.y >= 0);
        assert(body.x + body.width <= viewport.width + 0.5);
        assert(body.y + body.height <= viewport.height + 0.5);

        std::array<contracts::NodeId, 4> rows;
        for (std::size_t row = 0; row < rows.size(); ++row) {
            rows[row] = FindMenuAction(*shown, menu, commands[row]);
            assert(shown->Find(rows[row])->bounds.height == 36);
            if (row > 0 && row < 3) {
                const auto previous = shown->Find(rows[row - 1])->bounds;
                assert(std::abs(shown->Find(rows[row])->bounds.y - previous.y - 44) < 0.5);
            }
        }

        // Keyboard focus must reveal every command in a height-constrained BSP window.
        MenuKey(scene, shown, 0x4a); // Home.
        shown = Present(scene);
        for (std::size_t step = 0; step < index; ++step) {
            MenuKey(scene, shown, 0x51); // Down.
            shown = Present(scene);
        }
        assert(scene.State(rows[index]).focused);
        const auto bounds = shown->Find(rows[index])->bounds;
        assert(bounds.y >= body.y && bounds.y + bounds.height <= body.y + body.height);
        assert(scene.ActionAt({bounds.x + bounds.width / 2, bounds.y + bounds.height / 2}) ==
               commands[index]);
        const auto result = Click(scene, shown, rows[index]);
        assert(result.activation && result.activation->action == commands[index]);
        assert(!scene.PopupToken());
        shown = Present(scene);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::filesystem::path root = argv[1];
    runtime::TextShaper shaper("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    const auto shape = std::bind_front(
        static_cast<runtime::ShapedText (runtime::TextShaper::*)(std::string_view, double)>(
            &runtime::TextShaper::Shape),
        &shaper);
    const auto package = root / "demos/demo_player";
    const auto plan = runtime::CompileLoadPlan(Read(package / "master.prism"),
                                               {"master", "master.prism", "test"}, package);
    const auto layout = runtime::PrepareLayout(Read(package / "layout.prism"), plan);
    std::vector<runtime::PreparedUnit> units;
    std::vector<runtime::RegionUpdate> deferred;
    for (const auto &unit : plan.components) {
        auto prepared = runtime::PrepareComponent(Read(package / unit.source_path),
                                                  {unit.id, unit.source_path.string(), "test"});
        if (unit.phase == runtime::LoadPhase::Deferred) {
            deferred.push_back({unit.id, runtime::LinkComponent(prepared, ResolveImage)});
        }
        units.push_back({unit.id, std::move(prepared)});
    }
    const auto composed = runtime::ComposeCritical(plan, layout, units);
    for (const auto &theme : {"glass", "square", "translucent", "transparent"}) {
        for (const auto &scheme : {"light", "dark"}) {
            const auto snapshot = theme::LoadTheme(root / "resources/themes", theme, 1, scheme);
            for (const auto size : {contracts::LogicalSize{244, 420},
                                    {482, 204},
                                    {482, 420},
                                    {409, 540},
                                    {244, 540},
                                    {620, 540},
                                    {600, 540},
                                    {900, 640}}) {
                for (const auto &page : {"performance", "appearance", "system"}) {
                    runtime::Scene scene(
                        runtime::ParseBlueprint(Read(root / "demos/demo_settings/master.prism"),
                                                ResolveImage),
                        shape, shaper.FontId(), snapshot);
                    scene.SetViewport(size);
                    for (const auto &other : {"performance", "appearance", "system"}) {
                        scene.SetBinding(std::string("page_") + other, false);
                    }
                    scene.SetBinding("theme_error_visible", false);
                    scene.SetBinding("theme_normal", true);
                    scene.SetBinding(std::string("page_") + page, true);
                    scene.SetBinding("cpu_usage_text", std::string("35%"));
                    scene.SetBinding("mem_usage_text", std::string("1.2 / 3.7 GiB"));
                    scene.SetBinding("gpu_usage_text", std::string("500 MHz"));
                    Check(scene);
                    if (std::string_view(page) == "performance" &&
                        ((size.width == 244 && size.height == 420) ||
                         (size.width == 482 && size.height == 204) ||
                         (size.width == 600 && size.height == 540) ||
                         (size.width == 900 && size.height == 640))) {
                        CheckPreferencesMenu(scene, size);
                    }
                    scene.SetBinding("theme_normal", false);
                    scene.SetBinding("theme_error_visible", true);
                    scene.SetBinding("theme_status", std::string("Appearance unavailable"));
                    Check(scene);
                }
                runtime::Scene player(runtime::LinkComponent(composed, ResolveImage), shape,
                                      shaper.FontId(), snapshot);
                auto full = player.RegionBlueprint(deferred);
                runtime::Scene complete(std::move(full), shape, shaper.FontId(), snapshot);
                for (const auto &binding : plan.bindings) {
                    complete.SetBinding(binding.name, binding.initial);
                }
                complete.SetViewport(size);
                complete.SetBinding("library_empty", false);
                for (int row = 1; row <= 3; ++row) {
                    complete.SetBinding("library_row_" + std::to_string(row), true);
                    complete.SetBinding("library_title_" + std::to_string(row),
                                        std::string("Track"));
                }
                Check(complete);
                complete.SetBinding("catalog_error", true);
                complete.SetBinding("repeat_active", true);
                complete.SetBinding("favorite_active", false);
                Check(complete);
                complete.SetBinding("catalog_error", false);
                complete.SetBinding("artwork_visible", false);
                complete.SetBinding("library_visible", true);
                Check(complete);
                assert(!complete.Build({1}));
            }
        }
    }
}
