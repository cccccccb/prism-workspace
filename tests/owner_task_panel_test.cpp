#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "prism/runtime/text_buffer.hpp"
#include "prism/theme/compiler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace prism;
using namespace prism::runtime;

namespace {
const std::filesystem::path source_root{PRISM_SOURCE_ROOT};
constexpr contracts::WindowId window{1};

std::string Read(const std::filesystem::path &path)
{
    std::ifstream input(path);
    assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

ShapedText Shape(std::string_view text, double font)
{
    assert(TextBuffer::Valid(text));
    std::size_t scalars = 0;
    for (std::size_t at = 0; at < text.size(); at = TextBuffer::Next(text, at)) {
        ++scalars;
    }
    return {{}, scalars * font / 2, font};
}

ShapedText InvalidShape(std::string_view, double)
{
    return {{}, std::numeric_limits<double>::quiet_NaN(), 14};
}

Blueprint Shared()
{
    const auto prepared =
        PrepareComponent(Read(source_root / "resources/ui/owner-task-panel.prism"));
    assert(prepared.Images().empty());
    return LinkComponent(prepared);
}

Blueprint App(bool clip = false)
{
    auto root = ParseBlueprint(R"(
Card(material: "window", padding: "@window_padding") {
    VStack(spacing: 8, clip: true) {
        Button("Owner action", action: "owner", height: 32)
        Card(flex: 1) { Text("Current document", font: "@font_body", foreground: "@text") }
        Card(height: 32)
    }
    Menu("owner", width: 180, height: 100, material: "card", padding: 12) {
        MenuItem(action: "command", height: 32) { Text("Command", font: "@font_body", foreground: "@text") }
    }
})");
    root.properties.push_back({DslProperty::Clip, clip});
    root.children.front().children.back().region = "later";
    return root;
}

contracts::OwnerTaskRequest Request(bool two = true)
{
    contracts::OwnerTaskRequest request;
    request.request_id = 7;
    request.title = "Keep these changes?";
    request.message = "The current document has unsaved changes.\nChoose how to continue.";
    request.choices = {{11, "Discard", contracts::OwnerTaskChoiceRole::Destructive}};
    if (two) {
        request.choices.push_back({42, "Save", contracts::OwnerTaskChoiceRole::Primary});
    }
    assert(contracts::ValidateOwnerTaskRequest(request));
    return request;
}

contracts::NodeId Action(const Scene &scene, std::string_view action)
{
    const auto input = scene.InputGeometry();
    assert(input);
    for (const auto &node : input->nodes) {
        if (node.id && node.visible && node.action == action) {
            return node.id;
        }
    }
    return {};
}

template <std::size_t Count>
contracts::NodeId VisibleRegion(const Scene &scene,
                                const std::array<std::string_view, Count> &regions)
{
    contracts::NodeId result;
    for (const auto name : regions) {
        const auto id = scene.RegionId(name);
        assert(id && scene.RegionMounted(name));
        if (scene.IsVisible(id)) {
            assert(!result);
            result = id;
        }
    }
    assert(result);
    return result;
}

void Present(Scene &scene)
{
    if (const auto list = scene.Build(window)) {
        contracts::ValidateDisplayList(*list);
    }
    scene.ApplyInputSnapshot(scene.InputGeometry());
    scene.AcknowledgeComposite();
}

void CheckMeasurementRegionScope(const Blueprint &node, bool decorative = false)
{
    decorative = decorative || node.kind == Kind::Visual;
    assert(!decorative || node.region.empty());
    for (const auto &child : node.children) {
        CheckMeasurementRegionScope(child, decorative);
    }
}

void RootAndPopupContract()
{
    const auto shared = Shared();
    for (const bool clip : {false, true}) {
        const auto app = App(clip);
        const auto composed = ComposeOwnerTaskPanel(app, shared);
        assert(composed.kind == app.kind && composed.properties == app.properties);
        assert(composed.theme_refs == app.theme_refs);
        assert(composed.children.size() == app.children.size() + 1);
        assert(composed.children.front().kind == app.children.front().kind);
        assert(composed.children.back().kind == Kind::Menu);
        assert(composed.children.back().properties == app.children.back().properties);
        assert(HasOwnerTaskPanel(composed));
        CheckMeasurementRegionScope(composed);

        const auto theme = theme::LoadTheme(source_root / "resources/themes", "glass", 1);
        Scene before(app, Shape, {}, theme);
        Scene after(composed, Shape, {}, theme);
        before.SetViewport({640, 420});
        after.SetViewport({640, 420});
        assert(after.Preflight(OwnerTaskPanelDefaults()));
        Present(before);
        Present(after);
        assert(HasOwnerTaskPanel(after));
        assert(!after.IsVisible(after.RegionId(kOwnerTaskPanelRegion)));
        assert(before.Bounds(Action(before, "owner")) == after.Bounds(Action(after, "owner")));
        assert(before.Bounds(before.RegionId("later")) == after.Bounds(after.RegionId("later")));

        assert(before.OpenPopup(Action(before, "owner")));
        assert(after.OpenPopup(Action(after, "owner")));
        Present(before);
        Present(after);
        const auto first = before.CapturePopupSurfaceRequest(1);
        const auto second = after.CapturePopupSurfaceRequest(1);
        assert(first && second);
        assert(first->anchor == second->anchor &&
               first->desired_geometry == second->desired_geometry);
        const PopupSurfaceConfigure configured{1, 9, {14, 60, 180, 100}};
        const auto first_plan = before.PreparePopupSurface(*first, configured);
        const auto second_plan = after.PreparePopupSurface(*second, configured);
        assert(bool(first_plan) == bool(second_plan));
        if (!clip) {
            assert(first_plan && second_plan);
        }
    }
}

void LayoutAndModalMatrix()
{
    const auto shared = Shared();
    constexpr std::array sizes{contracts::LogicalSize{640, 420}, contracts::LogicalSize{244, 420},
                               contracts::LogicalSize{482, 204}, contracts::LogicalSize{244, 204}};
    for (const auto material : {"glass", "translucent", "transparent", "square"}) {
        for (const auto scheme : {"dark", "light"}) {
            const auto theme =
                theme::LoadTheme(source_root / "resources/themes", material, 1, scheme);
            for (const auto size : sizes) {
                for (const bool two : {false, true}) {
                    Scene scene(ComposeOwnerTaskPanel(App(), shared), Shape, {}, theme);
                    scene.SetViewport(size);
                    assert(scene.Preflight(OwnerTaskPanelDefaults()));
                    Present(scene);
                    const auto owner = Action(scene, "owner");
                    const auto owner_bounds = scene.Bounds(owner);
                    const auto request = Request(two);
                    assert(scene.Preflight(OwnerTaskPanelBindings(request)));
                    Present(scene);
                    assert(scene.Bounds(owner) == owner_bounds);
                    const auto panel = scene.RegionId(kOwnerTaskPanelRegion);
                    const auto token = scene.BeginOwnerModal(panel);
                    assert(token);
                    Present(scene);

                    assert(!scene.ActionAt({owner_bounds.x + 1, owner_bounds.y + 1}));
                    for (const auto action : {kOwnerTaskCancelAction, kOwnerTaskChoiceActions[0],
                                              kOwnerTaskChoiceActions[1]}) {
                        const auto id = Action(scene, action);
                        if (!two && action == kOwnerTaskChoiceActions[1]) {
                            assert(!id);
                            continue;
                        }
                        assert(id);
                        const auto bounds = scene.Bounds(id);
                        assert(bounds.width >= 32 && bounds.height >= 32);
                        assert(bounds.x >= 0 && bounds.y >= 0);
                        assert(bounds.x + bounds.width <= size.width);
                        assert(bounds.y + bounds.height <= size.height);
                        assert(scene.ActionAt({bounds.x + bounds.width / 2,
                                               bounds.y + bounds.height / 2}) == action);
                    }
                    const auto body = VisibleRegion(scene, kOwnerTaskBodyRegions);
                    const auto title = VisibleRegion(scene, kOwnerTaskTitleRegions);
                    const auto first_label = VisibleRegion(scene, kOwnerTaskChoiceLabelRegions[0]);
                    assert(scene.Bounds(body).width > 100 && scene.Bounds(title).width > 100);
                    assert(scene.Bounds(first_label).width > 30);
                    if (two) {
                        assert(scene.Bounds(VisibleRegion(scene, kOwnerTaskChoiceLabelRegions[1]))
                                   .width > 30);
                    }

                    const auto panel_id = scene.RegionId(kOwnerTaskPanelRegion);
                    const RegionUpdate update{
                        "later",
                        ParseBlueprint(R"(Button("Later", action:"later-action", height:32))")};
                    assert(scene.MountRegions(std::span(&update, 1), {}));
                    assert(scene.RegionId(kOwnerTaskPanelRegion) == panel_id);
                    assert(scene.OwnerModalToken() == *token);
                    Present(scene);
                    assert(scene.EndOwnerModal(*token));
                    assert(scene.Preflight(OwnerTaskPanelDefaults()));
                    Present(scene);
                    assert(scene.ActionAt({owner_bounds.x + 1, owner_bounds.y + 1}) == "owner");
                }
            }
        }
    }
}

std::string WithoutNewlines(std::string value)
{
    std::erase(value, '\n');
    return value;
}

void WrappingAndReadableOverflow()
{
    assert(WrapOwnerTaskText("", 80, 14, Shape).empty());
    assert(WrapOwnerTaskText("abc\n\n\tdef\n", 400, 14, Shape) == "abc\n\n def\n");
    assert(WrapOwnerTaskText("hello world", 56, 14, Shape) == "hello \nworld");
    for (const auto &text : {std::string("A long message with meaningful words and spaces"),
                             std::string("保持工作上下文完整，选择动作之前需要读完所有内容。"),
                             std::string("🎨 Theme ➜ appearance 𝄞"), std::string(2048, 'X')}) {
        const auto narrow = WrapOwnerTaskText(text, 35, 14, Shape);
        const auto wide = WrapOwnerTaskText(text, 280, 14, Shape);
        const auto tiny = WrapOwnerTaskText(text, .01, 14, Shape);
        assert(TextBuffer::Valid(narrow) && TextBuffer::Valid(wide) && TextBuffer::Valid(tiny));
        assert(WithoutNewlines(narrow) == text && WithoutNewlines(wide) == text);
        assert(WithoutNewlines(tiny) == text);
        assert(std::count(narrow.begin(), narrow.end(), '\n') >=
               std::count(wide.begin(), wide.end(), '\n'));
    }
    for (const auto width : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        bool rejected = false;
        try {
            WrapOwnerTaskText("message", width, 14, Shape);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    }
    for (const auto &text :
         {std::string("\x80"), std::string(2049, 'X'), std::string("bad\rtext")}) {
        bool rejected = false;
        try {
            WrapOwnerTaskText(text, 120, 14, Shape);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    }
    bool invalid_metrics = false;
    try {
        WrapOwnerTaskText("message", 120, 14, InvalidShape);
    } catch (const std::invalid_argument &) {
        invalid_metrics = true;
    }
    assert(invalid_metrics);

    auto request = Request();
    request.message = "";
    for (unsigned line = 0; line < 50; ++line) {
        request.message += "Keep all content in the task panel.\n";
    }
    const auto theme = theme::LoadTheme(source_root / "resources/themes", "glass", 1);
    Scene scene(ComposeOwnerTaskPanel(App(), Shared()), Shape, {}, theme);
    scene.SetViewport({482, 204});
    auto values = OwnerTaskPanelBindings(request);
    assert(scene.Preflight(values));
    Present(scene);
    const auto body = VisibleRegion(scene, kOwnerTaskBodyRegions);
    values.insert_or_assign(
        "__prism_task_message",
        WrapOwnerTaskText(request.message, scene.Bounds(body).width, 14, Shape));
    assert(scene.Preflight(values));
    Present(scene);
    const auto parent = scene.InputGeometry()->Find(body)->parent;
    const auto metrics = scene.ScrollInfo(parent);
    assert(metrics && metrics->maximum > 100);
    assert(scene.ScrollTo(parent, metrics->maximum));
    Present(scene);
    const auto content = scene.Bounds(body);
    const auto viewport = scene.Bounds(parent);
    assert(std::abs(content.y + content.height - viewport.y - viewport.height) < .01);
    assert(Action(scene, kOwnerTaskCancelAction));
    assert(Action(scene, kOwnerTaskChoiceActions[1]));
}

void ReservedAndUnsupported()
{
    const auto shared = Shared();
    auto row = ParseBlueprint(R"(HStack { Text("Unchanged") })");
    const auto original = row;
    row = ComposeOwnerTaskPanel(std::move(row), shared);
    assert(row.kind == original.kind && row.children.size() == original.children.size());
    assert(!HasOwnerTaskPanel(row));
    auto named = App();
    named.region = "application-root";
    assert(!HasOwnerTaskPanel(ComposeOwnerTaskPanel(named, shared)));

    for (unsigned mode = 0; mode < 6; ++mode) {
        auto app = App();
        auto &node = app.children.front().children.front();
        switch (mode) {
        case 0:
            node.region = "__prism_task_other";
            break;
        case 1:
            node.bindings.push_back({"__prism_task_title", DslProperty::Text});
            break;
        case 2:
            node.properties.push_back({DslProperty::Action, std::string(kOwnerTaskCancelAction)});
            break;
        case 3:
            node.properties.push_back(
                {DslProperty::PopupFor, std::string(kOwnerTaskChoiceActions[0])});
            break;
        case 4:
            node.gesture = GestureSpec{};
            node.gesture->action = kOwnerTaskCancelAction;
            break;
        case 5:
            node.state_rules.emplace_back();
            node.state_rules.back().properties.push_back(
                {DslProperty::Action, std::string(kOwnerTaskChoiceActions[1])});
            break;
        }
        bool rejected = false;
        try {
            ComposeOwnerTaskPanel(std::move(app), shared);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    }
    Blueprint maximum;
    maximum.children.resize(8191);
    bool budget = false;
    try {
        ComposeOwnerTaskPanel(std::move(maximum), shared);
    } catch (const std::length_error &) {
        budget = true;
    }
    assert(budget);
    auto bad_request = Request();
    bad_request.choices.front().id = 0;
    bool invalid_request = false;
    try {
        OwnerTaskPanelBindings(bad_request);
    } catch (const std::invalid_argument &) {
        invalid_request = true;
    }
    assert(invalid_request);
}
} // namespace

int main()
{
    RootAndPopupContract();
    LayoutAndModalMatrix();
    WrappingAndReadableOverflow();
    ReservedAndUnsupported();
    std::cout << "Owner task panel: composition, 64 theme/size/choice scenarios, modal, native "
                 "preservation and full text passed\n";
}
