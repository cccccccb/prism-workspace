#include "prism/compiler/lexer.hpp"
#include "prism/compiler/parser.hpp"
#include "prism/compiler/binary_generator.hpp"
#include "prism/compiler/binary_loader.hpp"
#include "prism/scene/container_node.hpp"
#include "prism/scene/leaf_nodes.hpp"
#include "prism/modifiers/acrylic_modifier.hpp"
#include "prism/modifiers/glow_modifier.hpp"
#include "prism/modifiers/spring_hover_modifier.hpp"
#include "prism/render/framebuffer.hpp"
#include "prism/render/canvas_renderer.hpp"
#include "prism/sdk/application.hpp"
#include "prism/wm/window.hpp"
#include "prism/core/logging.hpp"
#include <cassert>
#include <iostream>
#include <memory>
#include <filesystem>

using namespace prism;

void TestDslParsing() {
    std::cout << "[TEST] 1. Parsing Native DSL Widgets & GPU Modifiers...\n";
    std::string dsl_code = R"(
Card(spacing: 16.0) {
    HStack(spacing: 10.0) {
        Badge("ONLINE", $status_badge)
        Spacer(12.0)
        Toggle(isOn: true, $mute_switch)
    }
    TextInput(placeholder: "Type message...", $msg_input)
    ProgressBar(progress: 0.65, $progress_bar)
    ZStack {
        Text("Layer Bottom")
        Button("Send", "chat:send")
    }
}.acrylic(blur: 24.0, passes: 4, tint: #141822E6).glow(radius: 16.0, color: #007AFFE6).springOnHover(scale: 1.05, damping: 0.82)
)";

    compiler::Lexer lexer(dsl_code);
    auto tokens = lexer.Tokenize();
    assert(!tokens.empty());

    compiler::Parser parser(tokens);
    auto ast = parser.Parse();
    assert(ast != nullptr);
    assert(ast->type == compiler::BinaryNodeType::Card);
    assert(ast->children.size() == 4); // HStack, TextInput, ProgressBar, ZStack

    // Check HStack children
    auto hstack = ast->children[0];
    assert(hstack->type == compiler::BinaryNodeType::HStack);
    assert(hstack->children.size() == 3);
    assert(hstack->children[0]->type == compiler::BinaryNodeType::Badge);
    assert(hstack->children[0]->slot_binding == "status_badge");
    assert(hstack->children[0]->text_value == "ONLINE");

    assert(hstack->children[1]->type == compiler::BinaryNodeType::Spacer);
    assert(hstack->children[1]->numeric_value == 12.0f);

    assert(hstack->children[2]->type == compiler::BinaryNodeType::Toggle);
    assert(hstack->children[2]->slot_binding == "mute_switch");

    // Check TextInput
    auto input = ast->children[1];
    assert(input->type == compiler::BinaryNodeType::TextInput);
    assert(input->slot_binding == "msg_input");
    assert(input->text_value == "Type message...");

    // Check ProgressBar
    auto progress = ast->children[2];
    assert(progress->type == compiler::BinaryNodeType::ProgressBar);
    assert(progress->slot_binding == "progress_bar");
    assert(progress->numeric_value == 0.65f);

    // Check ZStack
    auto zstack = ast->children[3];
    assert(zstack->type == compiler::BinaryNodeType::ZStack);
    assert(zstack->children.size() == 2);

    // Check Modifiers
    assert(ast->modifiers.size() == 3);
    assert(ast->modifiers[0].name == "acrylic");
    assert(ast->modifiers[1].name == "glow");
    assert(ast->modifiers[2].name == "springOnHover");

    std::cout << "  -> DSL Parsing PASSED\n";
}

void TestBinarySerializationAndMmap() {
    std::cout << "[TEST] 2. AOT Binary Serialization & Zero-Copy mmap Loader Roundtrip...\n";
    std::string dsl_code = R"(
Card(spacing: 16.0) {
    HStack(spacing: 10.0) {
        Badge("PRO", $user_badge)
        Spacer(20.0)
        Toggle(false, $active_toggle)
    }
    TextInput("Search query...", $search_slot)
    ProgressBar(0.42, $load_slot)
    ZStack {
        Text("Background Layer")
        Button("Action", "act:click")
    }
}.acrylic(blur: 28.0, passes: 4, tint: #1E222EE6).glow(radius: 14.0, color: #3388FFEE).springOnHover(scale: 1.08, damping: 0.78)
)";

    compiler::Lexer lexer(dsl_code);
    auto tokens = lexer.Tokenize();
    compiler::Parser parser(tokens);
    auto ast = parser.Parse();
    assert(ast != nullptr);

    std::string test_prismb = "/tmp/prism_dsl_widgets_test.prismb";
    compiler::BinaryGenerator gen;
    bool write_ok = gen.WriteToFile(ast, test_prismb);
    assert(write_ok);

    auto loaded_root = compiler::BinarySceneLoader::LoadFromFile(test_prismb);
    assert(loaded_root != nullptr);

    auto card = std::dynamic_pointer_cast<scene::CardNode>(loaded_root);
    assert(card != nullptr);
    assert(card->GetChildren().size() == 4);

    // Verify HStack and children
    auto hstack = std::dynamic_pointer_cast<scene::HStackNode>(card->GetChildren()[0]);
    assert(hstack != nullptr);
    assert(hstack->GetChildren().size() == 3);

    auto badge = std::dynamic_pointer_cast<scene::BadgeNode>(hstack->GetChildren()[0]);
    assert(badge != nullptr);
    assert(badge->GetText() == "PRO");
    assert(badge->GetSlot() == core::HashSlot("$user_badge"));

    auto spacer = std::dynamic_pointer_cast<scene::SpacerNode>(hstack->GetChildren()[1]);
    assert(spacer != nullptr);
    assert(spacer->GetMinLength() == 20.0f);

    auto toggle = std::dynamic_pointer_cast<scene::ToggleNode>(hstack->GetChildren()[2]);
    assert(toggle != nullptr);
    assert(toggle->GetState() == false);
    assert(toggle->GetSlot() == core::HashSlot("$active_toggle"));

    // Verify TextInput
    auto input = std::dynamic_pointer_cast<scene::TextInputNode>(card->GetChildren()[1]);
    assert(input != nullptr);
    assert(input->GetPlaceholder() == "Search query...");
    assert(input->GetSlot() == core::HashSlot("$search_slot"));

    // Verify ProgressBar
    auto progress = std::dynamic_pointer_cast<scene::ProgressBarNode>(card->GetChildren()[2]);
    assert(progress != nullptr);
    assert(std::abs(progress->GetProgress() - 0.42) < 0.01);
    assert(progress->GetSlot() == core::HashSlot("$load_slot"));

    // Verify ZStack
    auto zstack = std::dynamic_pointer_cast<scene::ZStackNode>(card->GetChildren()[3]);
    assert(zstack != nullptr);
    assert(zstack->GetChildren().size() == 2);

    // Verify Modifiers
    const auto& mods = card->Modifiers().GetAll();
    assert(mods.size() >= 3);
    bool has_acrylic = false;
    bool has_glow = false;
    bool has_hover = false;
    for (const auto& mod : mods) {
        if (mod->GetType() == modifiers::ModifierType::Acrylic) {
            has_acrylic = true;
            auto acrylic = std::dynamic_pointer_cast<modifiers::AcrylicModifier>(mod);
            assert(acrylic != nullptr);
            assert(acrylic->GetBlurRadius() == 28.0f);
        } else if (mod->GetType() == modifiers::ModifierType::Glow) {
            has_glow = true;
            auto glow = std::dynamic_pointer_cast<modifiers::GlowModifier>(mod);
            assert(glow != nullptr);
            assert(glow->GetRadius() == 14.0f);
        } else if (mod->GetType() == modifiers::ModifierType::HoverSpring) {
            has_hover = true;
            auto hover = std::dynamic_pointer_cast<modifiers::HoverSpringModifier>(mod);
            assert(hover != nullptr);
            assert(hover->GetScale() == 1.08f);
        }
    }
    assert(has_acrylic);
    assert(has_glow);
    assert(has_hover);

    std::filesystem::remove(test_prismb);
    std::cout << "  -> AOT Binary Serialization & Zero-Copy mmap Loader Roundtrip PASSED\n";
}

void TestReactiveStateAndWindowBinding() {
    std::cout << "[TEST] 3. Reactive State Binding & Window Slot Synchronization...\n";
    std::string dsl_code = R"(
Card {
    HStack {
        Badge("Pending", $status)
        Toggle(false, $active)
    }
    TextInput("Name", $user_name)
    ProgressBar(0.10, $progress)
}
)";
    compiler::Lexer lexer(dsl_code);
    compiler::Parser parser(lexer.Tokenize());
    auto ast = parser.Parse();

    std::string path = "/tmp/prism_reactive_test.prismb";
    compiler::BinaryGenerator gen;
    gen.WriteToFile(ast, path);

    auto tree = compiler::BinarySceneLoader::LoadFromFile(path);
    assert(tree != nullptr);

    core::Rect bounds{50, 50, 600, 400};
    wm::Window win("app.demo", "Demo App", bounds, nullptr);
    win.SetMasterTree(tree);

    // Initial state checks
    auto card = std::dynamic_pointer_cast<scene::CardNode>(tree);
    auto hstack = std::dynamic_pointer_cast<scene::ContainerNode>(card->GetChildren()[0]);
    auto badge = std::dynamic_pointer_cast<scene::BadgeNode>(hstack->GetChildren()[0]);
    auto toggle = std::dynamic_pointer_cast<scene::ToggleNode>(hstack->GetChildren()[1]);
    auto input = std::dynamic_pointer_cast<scene::TextInputNode>(card->GetChildren()[1]);
    auto progress = std::dynamic_pointer_cast<scene::ProgressBarNode>(card->GetChildren()[2]);

    assert(badge->GetText() == "Pending");
    assert(toggle->GetState() == false);
    assert(input->GetText().empty());
    assert(std::abs(progress->GetProgress() - 0.10) < 0.01);

    // Apply reactive diffs via Window::UpdateSlot
    win.UpdateSlot(core::HashSlot("$status"), std::string("COMPLETED"));
    assert(badge->GetText() == "COMPLETED");

    win.UpdateSlot(core::HashSlot("$active"), true);
    assert(toggle->GetState() == true);

    win.UpdateSlot(core::HashSlot("$user_name"), std::string("Alice Cooper"));
    assert(input->GetText() == "Alice Cooper");

    win.UpdateSlot(core::HashSlot("$progress"), 0.85);
    assert(std::abs(progress->GetProgress() - 0.85) < 0.01);

    std::filesystem::remove(path);
    std::cout << "  -> Reactive State Binding & Window Slot Synchronization PASSED\n";
}

void TestCanvasVisualRasterization() {
    std::cout << "[TEST] 4. Canvas Renderer GPU/CPU Rasterization of New Widgets & Effects...\n";
    std::string dsl_code = R"(
Card(spacing: 12.0) {
    HStack(spacing: 8.0) {
        Badge("ACTIVE")
        Spacer(16.0)
        Toggle(true)
    }
    TextInput("Input something...")
    ProgressBar(0.72)
    ZStack {
        Text("Underneath")
        Button("Press Me", "test:btn")
    }
}.acrylic(blur: 16.0, passes: 4, tint: #1A202DE6).glow(radius: 12.0, color: #007AFFE6)
)";
    compiler::Lexer lexer(dsl_code);
    compiler::Parser parser(lexer.Tokenize());
    auto ast = parser.Parse();

    std::string path = "/tmp/prism_render_test.prismb";
    compiler::BinaryGenerator gen;
    gen.WriteToFile(ast, path);

    auto tree = compiler::BinarySceneLoader::LoadFromFile(path);
    assert(tree != nullptr);

    render::FrameBuffer fb(800, 600);
    fb.Clear(0x10141EE6);

    render::CanvasRenderVisitor visitor(fb, core::Rect{0, 0, 800, 600});
    tree->Accept(visitor);

    // Check that frame buffer was rendered into (has non-background pixels)
    bool has_drawn_pixels = false;
    size_t total_pixels = static_cast<size_t>(fb.GetWidth()) * static_cast<size_t>(fb.GetHeight());
    for (size_t i = 0; i < total_pixels; ++i) {
        if (fb.GetPixels()[i] != 0x10141EE6) {
            has_drawn_pixels = true;
            break;
        }
    }
    assert(has_drawn_pixels);

    std::filesystem::remove(path);
    std::cout << "  -> Canvas Renderer GPU/CPU Rasterization PASSED\n";
}

void TestSdkHotReloadPreservingState() {
    std::cout << "[TEST] 5. SDK Dynamic Hot-Reload Preserving Runtime State ($slots)...\n";
    std::string dsl_v1 = R"(
Card {
    HStack {
        Badge("V1", $badge)
        Toggle(false, $active)
    }
    TextInput("Type here...", $query)
    ProgressBar(0.2, $progress)
}
)";

    std::string dsl_v2 = R"(
Card {
    ZStack {
        Text("New Header")
    }
    HStack {
        Badge("V2", $badge)
        Spacer(16.0)
        Toggle(false, $active)
    }
    TextInput("New Search...", $query)
    ProgressBar(0.0, $progress)
}
)";

    std::string path_v1 = "/tmp/prism_hotreload_v1.prismb";
    std::string path_v2 = "/tmp/prism_hotreload_v2.prismb";

    compiler::BinaryGenerator gen;
    compiler::Lexer lex1(dsl_v1);
    compiler::Parser parse1(lex1.Tokenize());
    gen.WriteToFile(parse1.Parse(), path_v1);

    compiler::Lexer lex2(dsl_v2);
    compiler::Parser parse2(lex2.Tokenize());
    gen.WriteToFile(parse2.Parse(), path_v2);

    sdk::AppConfig cfg;
    cfg.app_id = "test_hotreload_app";
    cfg.package_path = path_v1;
    cfg.channel_name = "/tmp/prism_nonexistent_ch";
    cfg.width = 400;
    cfg.height = 300;

    auto app = sdk::Application::Create(cfg);
    assert(app != nullptr);

    // Initial check of v1 tree
    auto master1 = app->GetMasterTree();
    assert(master1 != nullptr);
    auto card1 = std::dynamic_pointer_cast<scene::CardNode>(master1);
    assert(card1 != nullptr);
    assert(card1->GetChildren().size() == 3); // HStack, TextInput, ProgressBar

    // Set live runtime state in backend app
    app->SetState("badge", "RUNNING");
    app->SetState("active", true);
    app->SetState("query", "Prism Fast Engine");
    app->SetState("progress", 0.75);

    // Trigger dynamic hot-reload to v2 layout!
    bool reload_ok = app->HotReload(path_v2);
    assert(reload_ok);

    // Inspect v2 tree
    auto master2 = app->GetMasterTree();
    assert(master2 != nullptr);
    auto card2 = std::dynamic_pointer_cast<scene::CardNode>(master2);
    assert(card2 != nullptr);
    assert(card2->GetChildren().size() == 4); // ZStack, HStack, TextInput, ProgressBar

    // Check ZStack in v2
    auto zstack = std::dynamic_pointer_cast<scene::ZStackNode>(card2->GetChildren()[0]);
    assert(zstack != nullptr);

    // Check that runtime values are 100% PRESERVED in v2 nodes!
    auto hstack2 = std::dynamic_pointer_cast<scene::ContainerNode>(card2->GetChildren()[1]);
    auto badge2 = std::dynamic_pointer_cast<scene::BadgeNode>(hstack2->GetChildren()[0]);
    auto toggle2 = std::dynamic_pointer_cast<scene::ToggleNode>(hstack2->GetChildren()[2]);
    auto input2 = std::dynamic_pointer_cast<scene::TextInputNode>(card2->GetChildren()[2]);
    auto progress2 = std::dynamic_pointer_cast<scene::ProgressBarNode>(card2->GetChildren()[3]);

    assert(badge2->GetText() == "RUNNING");
    assert(toggle2->GetState() == true);
    assert(input2->GetText() == "Prism Fast Engine");
    assert(std::abs(progress2->GetProgress() - 0.75) < 0.01);

    std::filesystem::remove(path_v1);
    std::filesystem::remove(path_v2);
    std::cout << "  -> SDK Dynamic Hot-Reload Preserving Runtime State PASSED\n";
}

#include "prism/gui/imgui_dsl_engine.hpp"

void TestImGuiDslEngineIntegration() {
    std::cout << "[TEST] 6. Declarative-to-Immediate ImGui DSL Deep Binding Engine...\n";

    std::string dsl_code = R"(
Dock(height: 72.0) {
    HStack(spacing: 12.0) {
        Button("田", "dock:launcher")
        Spacer(4.0)
        Button("Files", "app:launch:files")
        Button("Terminal", "app:launch:terminal")
        Button("Browser", "app:launch:browser")
        Spacer(4.0)
        Badge("● 3 Active", $running_badge)
    }
}.acrylic(blur: 28.0, passes: 4, tint: #181c26e6)
)";

    compiler::Lexer lexer(dsl_code);
    auto tokens = lexer.Tokenize();
    compiler::Parser parser(tokens);
    auto ast = parser.Parse();
    assert(ast != nullptr);

    gui::ImGuiDslEngine engine;
    assert(engine.Initialize(480, 72));

    // Test reactive slot binding
    engine.SetState("running_badge", "● 5 Active (Vector TTF)");
    assert(engine.GetState("running_badge") == "● 5 Active (Vector TTF)");

    std::string dispatched_action;
    engine.SetActionCallback([&](const std::string& act) {
        dispatched_action = act;
    });

    render::FrameBuffer fb(480, 72);
    fb.Clear(0x00000000);

    engine.RenderTree(ast, fb, 0.016f);

    // Verify non-empty rasterized pixel output (text and buttons drawn)
    bool has_pixels = false;
    for (int i = 0; i < 480 * 72; ++i) {
        if (fb.GetPixels()[i] != 0) {
            has_pixels = true;
            break;
        }
    }
    assert(has_pixels);

    engine.Shutdown();
    assert(!engine.IsInitialized());
    std::cout << "  -> ImGui DSL Deep Binding Engine PASSED!\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "  Project Prism - Native DSL Widgets & Effects Test Suite\n";
    std::cout << "========================================================\n";

    TestDslParsing();
    TestBinarySerializationAndMmap();
    TestReactiveStateAndWindowBinding();
    TestCanvasVisualRasterization();
    TestSdkHotReloadPreservingState();
    TestImGuiDslEngineIntegration();

    std::cout << "========================================================\n";
    std::cout << "  ALL NATIVE DSL WIDGET & EFFECT TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "========================================================\n";
    return 0;
}
