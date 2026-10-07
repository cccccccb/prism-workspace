#include "prism/contracts/display_list_validation.hpp"
#include "prism/runtime/owner_feedback_panel.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "prism/runtime/text_buffer.hpp"
#include "prism/theme/compiler.hpp"

#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace prism;

namespace {
const std::filesystem::path source_root{PRISM_SOURCE_ROOT};

std::string Read(const std::filesystem::path &path)
{
    std::ifstream file(path);
    assert(file);
    return {std::istreambuf_iterator<char>(file), {}};
}

runtime::ShapedText Shape(std::string_view text, double size)
{
    std::size_t count = 0;
    for (std::size_t at = 0; at < text.size(); at = runtime::TextBuffer::Next(text, at)) {
        ++count;
    }
    return {{}, count * size / 2, size};
}

runtime::Blueprint App()
{
    return runtime::ParseBlueprint(R"(
Card(material:"window",padding:"@window_padding") {
    VStack(clip:true) { Button("Owner",action:"owner",height:32) Card(flex:1) }
    Menu("owner",height:100,width:180) { MenuItem(action:"item",height:32) { Text("Item") } }
})");
}

contracts::OwnerFeedbackRequest Request(std::size_t count)
{
    contracts::OwnerFeedbackRequest request{19,      contracts::OwnerFeedbackKind::Success,
                                            "Saved", "Changes saved.\nContinue editing.",
                                            {},      0};
    if (count) {
        request.actions.push_back({1, "Undo"});
    }
    if (count == 2) {
        request.actions.push_back({2, "View"});
    }
    return request;
}

void CheckMatrix()
{
    const auto shared = runtime::LinkComponent(
        runtime::PrepareComponent(Read(source_root / "resources/ui/owner-feedback-panel.prism")));
    const auto task = runtime::LinkComponent(
        runtime::PrepareComponent(Read(source_root / "resources/ui/owner-task-panel.prism")));
    for (const auto material : {"glass", "translucent", "transparent", "square"}) {
        for (const auto scheme : {"light", "dark"}) {
            const auto theme =
                theme::LoadTheme(source_root / "resources/themes", material, 1, scheme);
            for (const contracts::LogicalSize size :
                 {contracts::LogicalSize{240, 180}, contracts::LogicalSize{640, 420}}) {
                for (std::size_t count = 0; count <= 2; ++count) {
                    const auto original = App();
                    auto composed = runtime::ComposeOwnerFeedbackPanel(original, shared);
                    composed = runtime::ComposeOwnerTaskPanel(std::move(composed), task);
                    assert(composed.properties == original.properties);
                    assert(composed.children.back().kind == runtime::Kind::Menu);
                    assert(composed.children[1].region == runtime::kOwnerFeedbackPanelRegion);
                    assert(composed.children[2].region == runtime::kOwnerTaskPanelRegion);
                    runtime::Scene scene(std::move(composed), Shape, {}, theme);
                    scene.SetViewport(size);
                    auto values = runtime::OwnerTaskPanelDefaults();
                    for (const auto &[name, value] : runtime::OwnerFeedbackPanelDefaults()) {
                        values.insert_or_assign(name, value);
                    }
                    assert(scene.PrepareDetached(values));
                    assert(runtime::HasOwnerFeedbackPanel(scene));
                    assert(!scene.IsVisible(scene.RegionId(runtime::kOwnerFeedbackPanelRegion)));
                    const auto epoch = scene.OwnerModalEpoch();
                    const auto request = Request(count);
                    for (const auto &[name, value] : runtime::OwnerFeedbackPanelBindings(request)) {
                        values.insert_or_assign(name, value);
                    }
                    const std::array updates{runtime::RegionUpdate{
                        std::string(runtime::kOwnerFeedbackPanelRegion),
                        runtime::InstantiateOwnerFeedbackPanel(shared, request, 1)}};
                    assert(scene.ReplaceRegions(updates, values));
                    const auto list = scene.Build({1});
                    assert(list);
                    contracts::ValidateDisplayList(*list);
                    assert(scene.OwnerModalEpoch() == epoch);
                    const auto input = scene.CaptureInputSnapshot();
                    const auto card =
                        input->Find(scene.RegionId(runtime::kOwnerFeedbackCardRegion));
                    assert(card && card->visible && card->bounds.width <= 336 &&
                           card->bounds.height <= 148);
                    assert(card->bounds.x >= 0 && card->bounds.y >= 0);
                    assert(card->bounds.x + card->bounds.width <= size.width);
                    assert(card->bounds.y + card->bounds.height <= size.height);
                    std::size_t controls = 0;
                    for (const auto &node : input->nodes) {
                        if (!node.visible || !runtime::IsOwnerFeedbackReservedName(node.action)) {
                            continue;
                        }
                        assert(node.interactive && node.bounds.height >= 32 &&
                               node.bounds.width >= 32);
                        assert(node.bounds.x >= card->bounds.x && node.bounds.y >= card->bounds.y);
                        assert(node.bounds.x + node.bounds.width <=
                               card->bounds.x + card->bounds.width);
                        assert(node.bounds.y + node.bounds.height <=
                               card->bounds.y + card->bounds.height);
                        ++controls;
                    }
                    assert(controls == count + 1);
                    assert(scene.TextLayoutInRegion(runtime::kOwnerFeedbackTitleRegion));
                    assert(scene.TextLayoutInRegion(runtime::kOwnerFeedbackMessageRegion));
                }
            }
        }
    }
}

bool AdjustCardHeight(runtime::Blueprint &node, double height)
{
    for (const auto &binding : node.bindings) {
        if (binding.name == "__prism_feedback_card_visible") {
            for (auto &property : node.properties) {
                if (property.id == runtime::DslProperty::Height) {
                    property.value = height;
                    return true;
                }
            }
            assert(false && "Shared feedback card height is absent");
        }
    }
    for (auto &child : node.children) {
        if (AdjustCardHeight(child, height)) {
            return true;
        }
    }
    return false;
}

void CheckExplicitCardMarker()
{
    auto shared = runtime::LinkComponent(
        runtime::PrepareComponent(Read(source_root / "resources/ui/owner-feedback-panel.prism")));
    assert(AdjustCardHeight(shared, 140));
    const auto theme = theme::LoadTheme(source_root / "resources/themes", "glass", 1);
    runtime::Scene scene(runtime::ComposeOwnerFeedbackPanel(App(), shared), Shape, {}, theme);
    scene.SetViewport({640, 420});
    auto values = runtime::OwnerFeedbackPanelDefaults();
    assert(std::get<bool>(values.at("__prism_feedback_card_visible")));
    assert(scene.PrepareDetached(values));
    const auto request = Request(1);
    values = runtime::OwnerFeedbackPanelBindings(request);
    const std::array updates{
        runtime::RegionUpdate{std::string(runtime::kOwnerFeedbackPanelRegion),
                              runtime::InstantiateOwnerFeedbackPanel(shared, request, 1)}};
    assert(scene.ReplaceRegions(updates, values));
    assert(scene.Build({1}));
    const auto card = scene.RegionId(runtime::kOwnerFeedbackCardRegion);
    assert(card && scene.RegionMounted(runtime::kOwnerFeedbackCardRegion));
    assert(scene.IsVisible(card) && scene.Bounds(card).height == 140);
}

void CheckReserved()
{
    const auto shared = runtime::LinkComponent(
        runtime::PrepareComponent(Read(source_root / "resources/ui/owner-feedback-panel.prism")));
    for (const std::string_view text :
         {"Card { Button(\"Bad\",action:\"__prism_feedback_event_1_2_3\") }",
          "Card { Text($__prism_feedback_title) }",
          "Card { InteractionTarget(action:\"owner\") {}.gesture(action:\"__prism_feedback_bad\") "
          "}"}) {
        bool rejected = false;
        try {
            runtime::ComposeOwnerFeedbackPanel(runtime::ParseBlueprint(text), shared);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    }
    auto root = App();
    root.region = "__prism_feedback_panel";
    bool rejected = false;
    try {
        runtime::ValidateOwnerFeedbackApplication(root);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
    runtime::Scene unsupported(
        runtime::ComposeOwnerFeedbackPanel(runtime::ParseBlueprint("VStack { Text(\"Original\") }"),
                                           shared),
        Shape);
    assert(!runtime::HasOwnerFeedbackPanel(unsupported));
}
} // namespace

int main()
{
    CheckReserved();
    CheckExplicitCardMarker();
    CheckMatrix();
}
