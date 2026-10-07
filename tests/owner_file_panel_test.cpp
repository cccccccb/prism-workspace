#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/owner_file_panel.hpp"
#include "prism/runtime/text_buffer.hpp"
#include "prism/theme/compiler.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
using namespace prism;
using namespace prism::runtime;
const std::filesystem::path root{PRISM_SOURCE_ROOT};

Blueprint Shared(std::string_view name)
{
    std::ifstream input(root / "resources/ui" / name);
    assert(input);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto prepared = PrepareComponent(source);
    assert(prepared.Images().empty());
    return LinkComponent(prepared);
}

ShapedText Shape(std::string_view text, double font)
{
    assert(TextBuffer::Valid(text));
    std::size_t count = 0;
    for (std::size_t at = 0; at < text.size(); at = TextBuffer::Next(text, at)) {
        ++count;
    }
    return {{}, count * font / 2, font};
}

Blueprint App()
{
    return ParseBlueprint(R"(
Card(material:"window", padding:14) {
    VStack(spacing:8) {
        Button("Document", action:"outside", height:32)
        Text("Work stays in the owner", flex:1)
    }
    Menu("outside", width:160, height:80) {
        MenuItem(action:"command", height:32) { Text("Command") }
    }
})");
}

void Present(Scene &scene)
{
    if (const auto list = scene.Build({1})) {
        contracts::ValidateDisplayList(*list);
    }
    scene.ApplyInputSnapshot(scene.InputGeometry());
    scene.AcknowledgeComposite();
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

void CheckRegions(const Blueprint &node, bool decorative = false)
{
    decorative = decorative || node.kind == Kind::Visual;
    assert(!decorative || node.region.empty());
    for (const auto &child : node.children) {
        CheckRegions(child, decorative);
    }
}

void CheckStatusViewport(const Scene &scene)
{
    bool found = false;
    for (const auto region : kOwnerFileStatusRegions) {
        if (const auto layout = scene.TextLayoutInRegion(region)) {
            assert(layout->height >= 22);
            found = true;
        }
    }
    assert(found);
}

OwnerFilePanelView View(contracts::OwnerTaskKind kind)
{
    OwnerFilePanelView view;
    view.title = "Choose a document";
    view.directory = "/home/user/Documents";
    view.filename = "notes.txt";
    view.status = "Select an item";
    view.page_caption = "1 / 2";
    view.selected_caption = "Selected: notes.txt";
    view.rows[0] = {"notes.txt", false, true};
    view.rows[1] = {"Resources", true, false};
    view.nav_enabled = true;
    view.submit_enabled = true;
    view.show_filename = kind == contracts::OwnerTaskKind::SaveFile;
    return view;
}

BindingValues Defaults()
{
    auto values = OwnerTaskPanelDefaults();
    for (const auto &[key, value] : OwnerFilePanelDefaults()) {
        values.insert_or_assign(key, value);
    }
    return values;
}

void GeometryAndModes()
{
    const auto confirmation = Shared("owner-task-panel.prism");
    const auto file = Shared("owner-file-panel.prism");
    const auto app = App();
    const auto combined = ComposeOwnerTaskPanels(app, &confirmation, &file);
    assert(combined.kind == app.kind && combined.properties == app.properties);
    assert(combined.children.size() == app.children.size() + 1);
    assert(combined.children.back().kind == Kind::Menu);
    assert(HasOwnerTaskPanel(combined));
    CheckRegions(combined);

    constexpr std::array sizes{contracts::LogicalSize{640, 420}, contracts::LogicalSize{320, 420},
                               contracts::LogicalSize{640, 240}, contracts::LogicalSize{320, 240}};
    constexpr std::array kinds{contracts::OwnerTaskKind::OpenFile,
                               contracts::OwnerTaskKind::SaveFile,
                               contracts::OwnerTaskKind::SelectDirectory};
    for (const auto material : {"glass", "translucent", "transparent", "square"}) {
        for (const auto scheme : {"dark", "light"}) {
            const auto theme = theme::LoadTheme(root / "resources/themes", material, 1, scheme);
            for (const auto size : sizes) {
                Scene original(app, Shape, {}, theme);
                original.SetViewport(size);
                Present(original);
                const auto outside = original.Bounds(Action(original, "outside"));
                for (const auto kind : kinds) {
                    Scene scene(combined, Shape, {}, theme);
                    scene.SetViewport(size);
                    assert(scene.Preflight(Defaults()));
                    Present(scene);
                    assert(scene.Bounds(Action(scene, "outside")) == outside);
                    assert(!scene.IsVisible(scene.RegionId(kOwnerTaskPanelRegion)));

                    auto view = View(kind);
                    assert(scene.Preflight(OwnerFilePanelBindings(kind, view)));
                    assert(!scene.BeginOwnerModal(scene.RegionId(kOwnerTaskPanelRegion)));
                    scene.ResolveLayout();
                    const auto token = scene.BeginOwnerModal(scene.RegionId(kOwnerTaskPanelRegion));
                    assert(token);
                    Present(scene);
                    assert(!Action(scene, kOwnerTaskChoiceActions[0]));
                    assert(Action(scene, kOwnerFileRowActions[0]));
                    assert(Action(scene, kOwnerFileRowActions[1]));
                    assert(!Action(scene, kOwnerFileRowActions[2]));
                    assert(scene.Bounds(Action(scene, kOwnerFileRowActions[0])).height == 32);
                    assert(bool(Action(scene, kOwnerFileNameAction)) == view.show_filename);
                    assert(bool(Action(scene, kOwnerFileHomeAction)) ==
                           (size.width >= 640 && size.height >= 400));
                    for (const auto action : {kOwnerTaskCancelAction, kOwnerFileSubmitAction}) {
                        const auto bounds = scene.Bounds(Action(scene, action));
                        assert(bounds.width >= 32 && bounds.height == 32);
                        assert(bounds.x >= 0 && bounds.y >= 0);
                        assert(bounds.x + bounds.width <= size.width);
                        assert(bounds.y + bounds.height <= size.height);
                    }
                    assert(!scene.ActionAt({outside.x + 1, outside.y + 1}));

                    view.loading = true;
                    view.submit_enabled = false;
                    view.nav_enabled = false;
                    view.rows = {};
                    view.status = "Loading directory";
                    assert(scene.Preflight(OwnerFilePanelBindings(kind, view)));
                    Present(scene);
                    CheckStatusViewport(scene);
                    assert(!Action(scene, kOwnerFileRowActions[0]));
                    assert(!scene.State(Action(scene, kOwnerFileSubmitAction)).enabled);
                    if (kind == contracts::OwnerTaskKind::SaveFile) {
                        assert(!scene.State(Action(scene, kOwnerFileNameAction)).enabled);
                        view.loading = false;
                        view.overwrite = true;
                        view.submit_enabled = true;
                        view.status = "Replace the existing file?";
                        assert(scene.Preflight(OwnerFilePanelBindings(kind, view)));
                        Present(scene);
                        assert(scene.OwnerModalToken() == *token);
                        assert(!Action(scene, kOwnerFileSubmitAction));
                        assert(Action(scene, kOwnerFileReplaceAction));
                        assert(Action(scene, kOwnerFileBackAction));
                        assert(scene.State(Action(scene, kOwnerFileBackAction)).enabled);
                        assert(!scene.State(Action(scene, kOwnerFileNameAction)).enabled);
                        const auto replace = scene.Bounds(Action(scene, kOwnerFileReplaceAction));
                        assert(replace.x + replace.width <= size.width);

                        view.loading = true;
                        view.status = "Checking existing file";
                        assert(scene.Preflight(OwnerFilePanelBindings(kind, view)));
                        Present(scene);
                        assert(!scene.State(Action(scene, kOwnerFileNameAction)).enabled);
                        assert(!scene.State(Action(scene, kOwnerFileBackAction)).enabled);
                        assert(!scene.State(Action(scene, kOwnerFileReplaceAction)).enabled);
                        assert(scene.State(Action(scene, kOwnerTaskCancelAction)).enabled);
                    }
                    assert(scene.EndOwnerModal(*token));
                    const contracts::OwnerTaskRequest confirm{
                        7,
                        contracts::OwnerTaskKind::Confirmation,
                        "Continue?",
                        "Current work stays visible",
                        {{11, "Continue", contracts::OwnerTaskChoiceRole::Primary}},
                        {}};
                    assert(scene.Preflight(OwnerTaskPanelBindings(confirm)));
                    Present(scene);
                    assert(Action(scene, kOwnerTaskChoiceActions[0]));
                    assert(!Action(scene, kOwnerFileRowActions[0]));
                    assert(!Action(scene, kOwnerFileSubmitAction));
                }
            }
        }
    }
}

void Validation()
{
    auto view = View(contracts::OwnerTaskKind::OpenFile);
    assert(ValidateOwnerFilePanelView(view));
    view.filename = std::string(4096, 'x');
    assert(ValidateOwnerFilePanelView(view));
    view.filename.push_back('x');
    assert(!ValidateOwnerFilePanelView(view));
    view = View(contracts::OwnerTaskKind::OpenFile);
    view.rows[0].name = std::string(256, 'x');
    assert(!ValidateOwnerFilePanelView(view));
    view = View(contracts::OwnerTaskKind::OpenFile);
    view.status = std::string(1025, 'x');
    assert(!ValidateOwnerFilePanelView(view));
    const auto file = Shared("owner-file-panel.prism");
    for (const auto mutation : {DslProperty::Action, DslProperty::Source}) {
        auto invalid = file;
        invalid.properties.push_back({mutation, std::string("untrusted")});
        bool rejected = false;
        try {
            ComposeOwnerTaskPanels(App(), nullptr, &invalid);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    }
    assert(HasOwnerTaskPanel(ComposeOwnerTaskPanels(App(), nullptr, &file)));
    auto reserved = App();
    reserved.children.front().bindings.push_back({"__prism_task_file_filename", DslProperty::Text});
    bool rejected = false;
    try {
        ComposeOwnerTaskPanels(std::move(reserved), nullptr, &file);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace

int main()
{
    GeometryAndModes();
    Validation();
    std::cout << "owner_file_panel_test: passed\n";
}
