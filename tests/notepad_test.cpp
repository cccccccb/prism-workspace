#include "../prism-notepad/notepad.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/load_plan.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_buffer.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>
#include <unistd.h>

using namespace prism;

namespace {
std::string Read(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

runtime::ShapedText Shape(std::string_view text, double size)
{
    runtime::ShapedText shaped;
    shaped.width = text.size() * size / 2;
    shaped.height = size;
    for (std::size_t i = 0; i < text.size(); ++i) {
        shaped.glyphs.push_back({static_cast<unsigned char>(text[i]), {i * size / 2, size}});
    }
    return shaped;
}

struct Host {
    std::map<std::string, std::string> strings;
    std::map<std::string, bool> booleans;
    std::vector<std::uint8_t> input;
    std::uint64_t id{};

    static int32_t Binding(void *context, PrismStringViewV1 key, PrismValueV1 value)
    {
        auto &self = *static_cast<Host *>(context);
        const std::string name(key.data, key.size);
        if (value.kind == PRISM_VALUE_STRING_V1) {
            self.strings[name] = std::string(value.as.string.data, value.as.string.size);
        } else if (value.kind == PRISM_VALUE_BOOL_V1) {
            self.booleans[name] = value.as.boolean;
        }
        return 0;
    }

    static int32_t Submit(void *context, const PrismWorkRequestV1 *request)
    {
        auto &self = *static_cast<Host *>(context);
        assert(request->input.size <= 65536);
        self.input.assign(request->input.data, request->input.data + request->input.size);
        self.id = request->task_id;
        return PRISM_WORK_ACCEPTED_V1;
    }

    void Complete(notepad::Notepad &app)
    {
        const auto job = nlohmann::json::from_cbor(input).get<notepad::FileJob>();
        const auto result = notepad::ProcessFile(job);
        const auto bytes = nlohmann::json::to_cbor(nlohmann::json(result));
        PrismWorkCompletionV1 completion{
            sizeof(completion), id, PRISM_WORK_SUCCEEDED_V1, 0, {bytes.data(), bytes.size()}, {}};
        app.Complete(completion);
    }
};

void Buffer()
{
    runtime::TextBuffer buffer;
    assert(buffer.Insert("abc\n你好"));
    assert(buffer.Key(0x2a, false, false, true));
    assert(buffer.Text() == "abc\n你");
    assert(buffer.Key(0x1d, true, false, true));
    assert(buffer.Text() == "abc\n你好");
    assert(buffer.Key(0x1d, true, true, true));
    assert(buffer.Text() == "abc\n你");
    buffer.Key(0x04, true, false, true);
    assert(buffer.Insert("replaced"));
    assert(buffer.Text() == "replaced");
    assert(!buffer.Insert(std::string(runtime::TextBuffer::MaxBytes, 'x')));
    assert(!runtime::TextBuffer::Valid(std::string("a\0b", 3)));
    assert(!runtime::TextBuffer::Valid("\xc0\xaf"));
    assert(!runtime::TextBuffer::Valid("\xed\xa0\x80"));
    assert(runtime::TextBuffer::Valid("日本語 🙂\r\n\t"));
    assert(buffer.Assign("a\r\nb"));
    buffer.Select(3);
    assert(buffer.Key(0x2a, false, false, true));
    assert(buffer.Text() == "ab");
    assert(buffer.Key(0x1d, true, false, true));
    assert(buffer.Text() == "a\r\nb");
    buffer.Select(1);
    assert(buffer.Key(0x28, false, false, true));
    assert(buffer.Text() == "a\r\n\r\nb");
}

void Scene()
{
    runtime::Scene scene(
        runtime::ParseBlueprint("TextArea($text, action: \"edit\", padding: 8, font: 16)"), Shape,
        contracts::ResourceId{1});
    assert(scene.SetViewport({320, 160}));
    assert(scene.Build({1}));
    contracts::PointerButtonEvent button;
    button.position = {15, 15};
    button.state = contracts::ButtonState::Pressed;
    scene.HandleInput(button, scene.InputGeometry());
    button.state = contracts::ButtonState::Released;
    assert(!scene.HandleInput(button, scene.InputGeometry()).activation);
    const auto typed =
        scene.HandleInput(contracts::TextInputEvent{{1}, "hello"}, scene.InputGeometry());
    assert(typed.text_edit && typed.text_edit->text == "hello");
    assert(!scene.SetBinding("text", std::string("hello")));
    contracts::KeyEvent key;
    key.state = contracts::ButtonState::Pressed;
    key.physical_key = 0x28;
    const auto newline = scene.HandleInput(key, scene.InputGeometry());
    assert(newline.text_edit && newline.text_edit->text == "hello\n");
    const auto list = scene.Build({1});
    assert(list);
    assert(!scene.Build({1}));
    scene.SetEnabled(scene.RootId(), false);
    assert(!scene.HandleInput(contracts::TextInputEvent{{1}, "ignored"}).text_edit);
}

int32_t CancelBeforeCommit(void *context)
{
    auto &calls = *static_cast<unsigned *>(context);
    return ++calls >= 3;
}

void Files(const std::filesystem::path &root)
{
    unsigned calls = 0;
    PrismWorkContextV1 cancelled{sizeof(cancelled), &calls, CancelBeforeCommit, nullptr, -1};
    const auto cancelled_path = (root / "cancelled.txt").string();
    assert(
        !notepad::ProcessFile({"save", cancelled_path, "cancel me", {}}, &cancelled).error.empty());
    assert(!std::filesystem::exists(cancelled_path));
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        assert(!entry.path().filename().string().starts_with(".prism-notepad-"));
    }
    const auto path = (root / "note.txt").string();
    auto saved = notepad::ProcessFile({"save", path, "hello\n你好", {}});
    assert(saved.error.empty() && saved.stamp.exists);
    auto opened = notepad::ProcessFile({"open", path, {}, {}});
    assert(opened.error.empty() && opened.text == "hello\n你好");
    auto collision = notepad::ProcessFile({"save", path, "overwrite", {}});
    assert(!collision.error.empty() && Read(path) == opened.text);
    auto update = notepad::ProcessFile({"save", path, "updated", opened.stamp});
    assert(update.error.empty() && Read(path) == "updated");
    assert(!notepad::ProcessFile({"save", path, "stale", opened.stamp}).error.empty());
    assert(!notepad::ProcessFile({"open", root.string(), {}, {}}).error.empty());
    std::filesystem::create_symlink(path, root / "link");
    assert(!notepad::ProcessFile({"open", (root / "link").string(), {}, {}}).error.empty());
    std::ofstream(root / "binary", std::ios::binary).write("a\0b", 3);
    assert(!notepad::ProcessFile({"open", (root / "binary").string(), {}, {}}).error.empty());
    std::ofstream(root / "large") << std::string(49153, 'x');
    assert(!notepad::ProcessFile({"open", (root / "large").string(), {}, {}}).error.empty());
    assert(
        !notepad::ProcessFile({"save", (root / "missing" / "a").string(), "x", {}}).error.empty());
}

void Application(const std::filesystem::path &root)
{
    Host host;
    PrismHostApiV1 api{};
    api.context = &host;
    api.set_binding = Host::Binding;
    api.submit_work = Host::Submit;
    notepad::Notepad app(&api);
    app.Edit("edit:0", "first");
    assert(!app.RequestClose());
    assert(host.booleans["confirm"]);
    app.Action("cancel");
    app.Edit("path", (root / "app.txt").string());
    app.Action("save");
    assert(!app.RequestClose());
    app.Edit("edit:0", "newer");
    host.Complete(app);
    assert(Read(root / "app.txt") == "first");
    assert(host.strings["details"].find("Unsaved") != std::string::npos);
    app.Action("save");
    host.Complete(app);
    assert(Read(root / "app.txt") == "newer");
    assert(app.RequestClose());
    app.Action("new");
    app.Edit("edit:1", "second");
    app.Action("previous");
    assert(host.strings["text_0"] == "newer" && host.strings["text_1"] == "second");
    app.Action("next");
    app.Action("close");
    assert(host.booleans["confirm"]);
    app.Action("cancel");
    assert(host.strings["text_1"] == "second");
    app.Action("close");
    app.Action("discard");
    assert(!host.booleans["used_1"]);
    assert(app.RequestClose());
}

void Tabs(const std::filesystem::path &root)
{
    Host host;
    PrismHostApiV1 api{};
    api.context = &host;
    api.set_binding = Host::Binding;
    api.submit_work = Host::Submit;
    notepad::Notepad app(&api);
    app.Action("new");
    app.Action("new");
    assert(host.booleans["tab_2"] && !host.booleans["tab_0"]);
    app.Action("page-previous");
    assert(host.booleans["tab_0"] && host.booleans["tab_1"]);
    app.Action("close:0");
    assert(!host.booleans["used_0"] && host.booleans["selected_2"]);
    app.Edit("edit:1", "background draft");
    app.Action("close:1");
    assert(host.booleans["confirm"] && !host.booleans["normal"]);
    app.Action("select:2");
    app.Action("new");
    assert(host.booleans["confirm"] && !host.booleans["used_0"]);
    app.Action("cancel");
    assert(host.booleans["selected_2"] && host.booleans["used_1"]);
    app.Action("close:1");
    app.Action("save-close");
    assert(host.booleans["path_panel"]);
    app.Edit("path", (root / "tabs.txt").string());
    app.Action("apply-path");
    app.Edit("edit:1", "newer draft");
    host.Complete(app);
    assert(Read(root / "tabs.txt") == "background draft");
    assert(host.booleans["used_1"] && host.booleans["confirm"]);
    app.Action("save-close");
    host.Complete(app);
    assert(Read(root / "tabs.txt") == "newer draft");
    assert(!host.booleans["used_1"] && host.booleans["selected_2"]);
    app.Edit("edit:2", "keep me");
    app.Action("close:2");
    app.Action("save-close");
    app.Edit("path", (root / "missing" / "fail.txt").string());
    app.Action("apply-path");
    host.Complete(app);
    assert(host.booleans["used_2"] && host.strings["text_2"] == "keep me");
    app.Action("cancel");
    app.Action("close:2");
    app.Action("discard");
    assert(host.booleans["used_0"] && host.strings["text_0"].empty());
    for (int i = 0; i < 9; ++i) {
        app.Action("new");
    }
    assert(host.booleans["used_7"]);
    assert(host.strings["status"].find("Eight") != std::string::npos);
}

void Package(const std::filesystem::path &root)
{
    const auto plan = runtime::CompileLoadPlan(
        Read(root / "master.prism"), {"master", (root / "master.prism").string(), "test"}, root);
    const auto layout = runtime::PrepareLayout(Read(root / "layout.prism"), plan);
    (void)layout;
    for (const auto &name : {"toolbar", "editor", "status"}) {
        const auto path = root / "ui" / (std::string(name) + ".prism");
        const auto component = runtime::PrepareComponent(Read(path), {name, path.string(), "test"});
        runtime::ValidatePreparedUnit(plan, name, component);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    char temporary[] = "/tmp/prism-notepad-test-XXXXXX";
    assert(mkdtemp(temporary));
    Buffer();
    Scene();
    Files(temporary);
    Application(temporary);
    Tabs(temporary);
    Package(argv[1]);
    std::filesystem::remove_all(temporary);
}
