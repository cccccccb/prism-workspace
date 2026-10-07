#include "../prism-notepad/notepad.hpp"
#include "prism/runtime/dsl_frontend.hpp"
#include "prism/runtime/load_plan.hpp"
#include "prism/runtime/scene.hpp"
#include "prism/runtime/text_buffer.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
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
    std::uint64_t work_id{}, task_id{}, sequence{};
    std::uint32_t capabilities =
        PRISM_TASK_CAP_CONFIRMATION_V1 | PRISM_TASK_CAP_OPEN_FILE_V1 | PRISM_TASK_CAP_SAVE_FILE_V1;
    std::uint32_t kind{};
    std::string title, message, directory, suggested;
    std::vector<std::pair<std::uint32_t, std::string>> choices;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> close_results;
    bool reject_submit{}, reject_task{}, throw_binding{}, throw_submit{};
    std::vector<std::uint64_t> cancelled_tasks;
    std::uint32_t feedback_capabilities{PRISM_FEEDBACK_CAP_OWNER_V1}, feedback_kind{},
        feedback_duration{};
    std::uint64_t feedback_id{}, feedback_sequence{};
    std::size_t feedback_queries{}, feedback_shown{};
    std::string feedback_title, feedback_message;
    std::vector<std::pair<std::uint32_t, std::string>> feedback_actions;
    std::vector<std::uint64_t> dismissed_feedback;
    bool throw_feedback{}, reject_feedback{};

    static int32_t Binding(void *context, PrismStringViewV1 key, PrismValueV1 value)
    {
        auto &self = *static_cast<Host *>(context);
        if (self.throw_binding) {
            self.throw_binding = false;
            throw std::runtime_error("Transient binding failure");
        }
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
        assert(request->input.size <= 65536 && !self.work_id);
        if (self.throw_submit) {
            self.throw_submit = false;
            throw std::runtime_error("Transient work submission failure");
        }
        if (self.reject_submit) {
            return PRISM_WORK_BUSY_V1;
        }
        self.input.assign(request->input.data, request->input.data + request->input.size);
        self.work_id = request->task_id;
        return PRISM_WORK_ACCEPTED_V1;
    }

    static std::uint32_t Capabilities(void *context)
    {
        return static_cast<Host *>(context)->capabilities;
    }

    static std::uint64_t Request(void *context, const PrismTaskRequestV1 *request)
    {
        auto &self = *static_cast<Host *>(context);
        assert(request && request->struct_size == sizeof(*request) && !self.task_id);
        if (self.reject_task) {
            return 0;
        }
        self.kind = request->kind;
        self.title.assign(request->title.data, request->title.size);
        self.message = request->message.size
                           ? std::string(request->message.data, request->message.size)
                           : std::string{};
        self.choices.clear();
        for (std::size_t i = 0; i < request->choices_size; ++i) {
            const auto &choice = request->choices[i];
            self.choices.emplace_back(choice.id, std::string(choice.label.data, choice.label.size));
        }
        self.directory.clear();
        self.suggested.clear();
        if (request->file) {
            assert(request->file->struct_size == sizeof(*request->file));
            assert(!request->choices_size);
            const auto &file = *request->file;
            if (file.initial_directory.size) {
                self.directory.assign(file.initial_directory.data, file.initial_directory.size);
            }
            if (file.suggested_name.size) {
                self.suggested.assign(file.suggested_name.data, file.suggested_name.size);
            }
        }
        self.task_id = ++self.sequence;
        return self.task_id;
    }

    static int32_t Cancel(void *context, std::uint64_t id)
    {
        auto &self = *static_cast<Host *>(context);
        assert(id == self.task_id);
        self.cancelled_tasks.push_back(id);
        return 0;
    }

    static int32_t Close(void *context, std::uint64_t id, std::uint32_t decision)
    {
        auto &self = *static_cast<Host *>(context);
        assert(id && (decision == PRISM_CLOSE_ACCEPT_V1 || decision == PRISM_CLOSE_REJECT_V1));
        self.close_results.emplace_back(id, decision);
        return PRISM_CLOSE_COMPLETED_V1;
    }

    static std::uint32_t FeedbackCapabilities(void *context)
    {
        auto &self = *static_cast<Host *>(context);
        ++self.feedback_queries;
        return self.feedback_capabilities;
    }

    static std::uint64_t Feedback(void *context, const PrismFeedbackRequestV1 *request)
    {
        auto &self = *static_cast<Host *>(context);
        if (self.throw_feedback) {
            self.throw_feedback = false;
            throw std::runtime_error("Optional feedback publication failed");
        }
        if (self.reject_feedback) {
            return 0;
        }
        assert(request && request->struct_size == sizeof(*request));
        assert(request->title.size && request->title.size <= 256 && request->message.size <= 2048);
        assert(request->actions_size <= 2);
        self.feedback_title.assign(request->title.data, request->title.size);
        self.feedback_message.assign(request->message.data, request->message.size);
        self.feedback_kind = request->kind;
        self.feedback_duration = request->duration_ms;
        self.feedback_actions.clear();
        for (std::size_t i = 0; i < request->actions_size; ++i) {
            const auto &action = request->actions[i];
            assert(action.struct_size == sizeof(action) && action.id && action.label.size <= 48);
            self.feedback_actions.emplace_back(action.id,
                                               std::string(action.label.data, action.label.size));
        }
        ++self.feedback_shown;
        self.feedback_id = ++self.feedback_sequence;
        return self.feedback_id;
    }

    static int32_t DismissFeedback(void *context, std::uint64_t id)
    {
        auto &self = *static_cast<Host *>(context);
        self.dismissed_feedback.push_back(id);
        if (id != self.feedback_id) {
            return PRISM_FEEDBACK_INVALID_V1;
        }
        self.feedback_id = 0;
        return PRISM_FEEDBACK_DISMISSED_V1;
    }

    void FeedbackAction(notepad::Notepad &app, std::uint64_t id, std::uint32_t action)
    {
        if (id == feedback_id) {
            feedback_id = 0; // Host retires explicit action before business callback.
        }
        const PrismFeedbackActionEventV1 event{sizeof(event), id, action};
        const auto *module = prism_app_module_v1();
        assert(module->on_feedback_action);
        module->on_feedback_action(&app, &event);
    }

    PrismHostApiV1 Api()
    {
        PrismHostApiV1 api{};
        api.struct_size = sizeof(api);
        api.abi_version = PRISM_APP_ABI_V1;
        api.context = this;
        api.set_binding = Binding;
        api.submit_work = Submit;
        api.task_capabilities = Capabilities;
        api.request_task = Request;
        api.cancel_task = Cancel;
        api.complete_close = Close;
        api.feedback_capabilities = FeedbackCapabilities;
        api.show_feedback = Feedback;
        api.dismiss_feedback = DismissFeedback;
        return api;
    }

    void Complete(notepad::Notepad &app, std::uint32_t status = PRISM_WORK_SUCCEEDED_V1)
    {
        assert(work_id);
        const auto id = work_id;
        const auto job = nlohmann::json::from_cbor(input).get<notepad::FileJob>();
        const auto result =
            status == PRISM_WORK_SUCCEEDED_V1 ? notepad::ProcessFile(job) : notepad::FileResult{};
        const auto bytes = nlohmann::json::to_cbor(nlohmann::json(result));
        work_id = 0;
        input.clear();
        PrismWorkCompletionV1 completion{sizeof(completion),           id, status, 0,
                                         {bytes.data(), bytes.size()}, {}};
        app.Complete(completion);
    }

    void Result(notepad::Notepad &app, std::uint32_t outcome, std::uint32_t choice = 0,
                std::string path = {}, bool overwrite = false)
    {
        assert(task_id);
        PrismTaskResultV1 result{};
        result.struct_size = sizeof(result);
        result.request_id = task_id;
        result.outcome = outcome;
        result.choice_id = choice;
        result.cancel_reason = outcome == PRISM_TASK_CANCELLED_V1 ? PRISM_TASK_CANCEL_USER_V1 : 0;
        result.failure_code = outcome == PRISM_TASK_FAILED_V1 ? PRISM_TASK_OPERATION_FAILED_V1 : 0;
        result.file_path = {path.data(), path.size()};
        result.overwrite_approved = overwrite;
        task_id = 0; // Host retires before the callback can request a successor.
        app.TaskComplete(result);
    }

    void Choose(notepad::Notepad &app, std::uint32_t choice)
    {
        assert(kind == PRISM_TASK_CONFIRMATION_V1);
        Result(app, PRISM_TASK_SUCCEEDED_V1, choice);
    }

    void File(notepad::Notepad &app, const std::filesystem::path &path, bool overwrite = false)
    {
        assert(kind == PRISM_TASK_OPEN_FILE_V1 || kind == PRISM_TASK_SAVE_FILE_V1);
        Result(app, PRISM_TASK_SUCCEEDED_V1, 0, path.string(), overwrite);
    }
};

int32_t Close(notepad::Notepad &app, std::uint64_t id)
{
    return app.RequestClose({sizeof(PrismCloseRequestV1), id});
}

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
    auto api = host.Api();
    notepad::Notepad app(&api);
    app.Edit("edit:0", "first");
    app.Action("save");
    assert(host.kind == PRISM_TASK_SAVE_FILE_V1 && host.suggested == "Untitled.txt");
    assert(!host.booleans["available"] && host.booleans["active_0"]);
    app.Action("new");
    app.Edit("edit:0", "ignored during selection");
    assert(!host.booleans["used_1"] && host.strings["text_0"] == "first");
    assert(Close(app, 1) == PRISM_CLOSE_REJECT_V1);
    host.File(app, root / "app.txt");
    assert(host.work_id && host.booleans["editable"] && !host.booleans["available"]);
    app.Edit("edit:0", "newer");
    host.Complete(app);
    assert(Read(root / "app.txt") == "first");
    assert(host.strings["details"].find("Unsaved") != std::string::npos);
    app.Action("save");
    assert(!host.task_id);
    host.Complete(app);
    assert(Read(root / "app.txt") == "newer");
    assert(Close(app, 2) == PRISM_CLOSE_ACCEPT_V1 && host.close_results.empty());

    app.Action("new");
    app.Edit("edit:1", "second");
    app.Action("previous");
    assert(host.strings["text_0"] == "newer" && host.strings["text_1"] == "second");
    app.Action("next");
    app.Action("close");
    assert(host.kind == PRISM_TASK_CONFIRMATION_V1 && host.choices.size() == 2);
    assert(host.choices[0] == std::make_pair(1u, std::string("Save")));
    assert(host.choices[1] == std::make_pair(2u, std::string("Discard")));
    host.Result(app, PRISM_TASK_CANCELLED_V1);
    assert(host.strings["text_1"] == "second" && host.booleans["used_1"]);
    app.Action("close");
    host.Choose(app, 2);
    assert(!host.booleans["used_1"] && host.booleans["selected_0"]);
    assert(Close(app, 3) == PRISM_CLOSE_ACCEPT_V1);
}

void Tabs(const std::filesystem::path &root)
{
    Host host;
    auto api = host.Api();
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
    app.Action("select:2");
    app.Action("new");
    assert(host.task_id && !host.booleans["used_0"] && host.booleans["selected_1"]);
    host.Result(app, PRISM_TASK_CANCELLED_V1);
    assert(host.booleans["used_1"] && host.strings["text_1"] == "background draft");
    app.Action("close:1");
    host.Choose(app, 1);
    assert(host.kind == PRISM_TASK_SAVE_FILE_V1);
    host.File(app, root / "tabs.txt");
    app.Edit("edit:1", "newer draft");
    host.Complete(app);
    assert(Read(root / "tabs.txt") == "background draft");
    assert(host.booleans["used_1"] && host.kind == PRISM_TASK_CONFIRMATION_V1);
    host.Choose(app, 1);
    host.Complete(app);
    assert(Read(root / "tabs.txt") == "newer draft");
    assert(!host.booleans["used_1"] && host.booleans["selected_2"]);

    app.Edit("edit:2", "keep me");
    app.Action("close:2");
    host.Choose(app, 1);
    host.File(app, root / "missing" / "fail.txt");
    host.Complete(app);
    assert(host.booleans["used_2"] && host.strings["text_2"] == "keep me");
    assert(!host.task_id && host.booleans["available"]);
    app.Action("close:2");
    host.Choose(app, 2);
    assert(host.booleans["used_0"] && host.strings["text_0"].empty());
    for (int i = 0; i < 9; ++i) {
        app.Action("new");
    }
    assert(host.booleans["used_7"]);
    assert(host.strings["status"].find("Eight") != std::string::npos);
}

void OpenAndSaveAs(const std::filesystem::path &root)
{
    const auto path = root / "open.txt";
    std::ofstream(path) << "existing";
    Host host;
    auto api = host.Api();
    notepad::Notepad app(&api);
    app.Edit("edit:0", "draft");
    app.Action("open-panel");
    host.Result(app, PRISM_TASK_CANCELLED_V1);
    assert(!host.work_id && !host.booleans["used_1"] && host.strings["text_0"] == "draft");
    app.Action("open-panel");
    host.File(app, path);
    host.Complete(app);
    assert(host.strings["text_1"] == "existing" && host.strings["text_0"] == "draft");
    app.Action("open-panel");
    assert(host.directory == root.string());
    host.File(app, path);
    assert(!host.work_id && !host.booleans["used_2"] && host.booleans["selected_1"]);

    app.Action("select:0");
    app.Action("save-as");
    host.File(app, path, true);
    assert(!host.work_id && host.strings["text_0"] == "draft");
    assert(host.strings["status"].find("another document") != std::string::npos);
    const auto target = root / "overwrite.txt";
    std::ofstream(target) << "old";
    app.Action("save-as");
    host.File(app, target);
    host.Complete(app);
    assert(Read(target) == "old" && host.strings["path"].empty());
    app.Action("save-as");
    host.File(app, target, true);
    host.Complete(app);
    assert(Read(target) == "draft" && host.strings["path"] == target.string());

    app.Edit("edit:0", "changed");
    std::ofstream(target) << "external";
    app.Action("save");
    host.Complete(app);
    assert(Read(target) == "external" && host.strings["text_0"] == "changed");
    for (int i = 0; i < 8; ++i) {
        app.Action("new");
    }
    app.Action("open-panel");
    host.File(app, root / "app.txt");
    assert(!host.work_id && host.strings["status"].find("maximum eight") != std::string::npos);
}

void OwnerClose(const std::filesystem::path &root)
{
    Host host;
    auto api = host.Api();
    notepad::Notepad app(&api);
    app.Edit("edit:0", "first draft");
    app.Action("new");
    app.Edit("edit:1", "second draft");
    assert(Close(app, 11) == PRISM_CLOSE_DEFER_V1);
    assert(host.kind == PRISM_TASK_CONFIRMATION_V1 && host.booleans["selected_0"]);
    host.Choose(app, 2);
    assert(host.kind == PRISM_TASK_CONFIRMATION_V1 && host.booleans["selected_1"]);
    assert(host.booleans["used_0"] && host.strings["text_0"] == "first draft");
    host.Result(app, PRISM_TASK_CANCELLED_V1);
    assert((host.close_results ==
            std::vector<std::pair<std::uint64_t, std::uint32_t>>({{11, PRISM_CLOSE_REJECT_V1}})));
    assert(host.booleans["used_0"] && host.booleans["used_1"] && host.booleans["available"]);

    assert(Close(app, 12) == PRISM_CLOSE_DEFER_V1);
    host.Choose(app, 1);
    assert(host.kind == PRISM_TASK_SAVE_FILE_V1);
    host.File(app, root / "close-first.txt");
    app.Edit("edit:0", "newer first");
    host.Complete(app);
    assert(Read(root / "close-first.txt") == "first draft");
    assert(host.kind == PRISM_TASK_CONFIRMATION_V1 && host.booleans["selected_0"]);
    host.Choose(app, 1);
    host.Complete(app);
    assert(Read(root / "close-first.txt") == "newer first");
    assert(host.kind == PRISM_TASK_CONFIRMATION_V1 && host.booleans["selected_1"]);
    host.Choose(app, 2);
    assert(host.close_results.size() == 2 &&
           host.close_results.back() ==
               std::make_pair(std::uint64_t{12}, std::uint32_t{PRISM_CLOSE_ACCEPT_V1}));
    assert(host.booleans["used_0"] && host.booleans["used_1"]);
    assert(!host.task_id && !host.work_id);
}

void CloseFailures(const std::filesystem::path &root)
{
    Host host;
    auto api = host.Api();
    notepad::Notepad app(&api);
    app.Edit("edit:0", "retain");
    assert(Close(app, 21) == PRISM_CLOSE_DEFER_V1);
    host.Choose(app, 1);
    host.Result(app, PRISM_TASK_CANCELLED_V1);
    assert(host.close_results.back().second == PRISM_CLOSE_REJECT_V1);
    assert(host.strings["text_0"] == "retain" && host.booleans["available"]);
    assert(Close(app, 22) == PRISM_CLOSE_DEFER_V1);
    host.Choose(app, 1);
    host.File(app, root / "missing" / "close.txt");
    host.Complete(app);
    assert(host.close_results.back() ==
           std::make_pair(std::uint64_t{22}, std::uint32_t{PRISM_CLOSE_REJECT_V1}));
    assert(host.strings["text_0"] == "retain");

    app.Action("save");
    host.File(app, root / "busy-close.txt");
    assert(Close(app, 23) == PRISM_CLOSE_DEFER_V1 && !host.task_id);
    app.Action("new");
    app.Edit("edit:0", "newer while closing");
    host.Complete(app);
    assert(host.task_id && host.kind == PRISM_TASK_CONFIRMATION_V1);
    host.Choose(app, 1);
    host.Complete(app);
    assert(Read(root / "busy-close.txt") == "newer while closing");
    assert(host.close_results.back() ==
           std::make_pair(std::uint64_t{23}, std::uint32_t{PRISM_CLOSE_ACCEPT_V1}));
    assert(!host.booleans["used_1"]);

    app.Edit("edit:0", "cancelled work draft");
    app.Action("save");
    assert(Close(app, 24) == PRISM_CLOSE_DEFER_V1);
    host.Complete(app, PRISM_WORK_CANCELLED_V1);
    assert(host.close_results.back().second == PRISM_CLOSE_REJECT_V1);
    assert(host.strings["text_0"] == "cancelled work draft" && host.booleans["available"]);

    host.capabilities = 0;
    app.Action("open-panel");
    assert(!host.task_id && host.strings["status"].find("unavailable") != std::string::npos);
    assert(Close(app, 25) == PRISM_CLOSE_DEFER_V1);
    assert(host.close_results.back() ==
           std::make_pair(std::uint64_t{25}, std::uint32_t{PRISM_CLOSE_REJECT_V1}));
    assert(host.booleans["used_0"] && host.strings["text_0"] == "cancelled work draft");
    api.struct_size = offsetof(PrismHostApiV1, task_capabilities);
    assert(Close(app, 26) == PRISM_CLOSE_REJECT_V1);
    assert(!app.RequestLegacyClose());
}

void BadAndStaleResults(const std::filesystem::path &root)
{
    Host host;
    auto api = host.Api();
    notepad::Notepad app(&api);
    app.Edit("edit:0", "draft");
    app.Action("save");
    const auto first = host.task_id;
    host.Result(app, PRISM_TASK_CANCELLED_V1);
    app.Action("save");
    const auto second = host.task_id;
    PrismTaskResultV1 stale{};
    stale.struct_size = sizeof(stale);
    stale.request_id = first;
    stale.outcome = PRISM_TASK_SUCCEEDED_V1;
    const auto path = (root / "stale.txt").string();
    stale.file_path = {path.data(), path.size()};
    app.TaskComplete(stale);
    assert(host.task_id == second && !host.work_id);
    stale.request_id = second;
    stale.struct_size = offsetof(PrismTaskResultV1, file_path);
    host.task_id = 0;
    app.TaskComplete(stale);
    assert(!host.work_id && host.booleans["available"]);

    app.Action("save");
    host.File(app, root / "malformed.txt");
    const auto work = host.work_id;
    const std::uint8_t invalid[] = {0xff};
    PrismWorkCompletionV1 bad{
        sizeof(bad), work, PRISM_WORK_SUCCEEDED_V1, 0, {invalid, sizeof(invalid)}, {}};
    host.work_id = 0;
    app.Complete(bad);
    assert(host.strings["text_0"] == "draft" && host.booleans["available"]);
    app.Action("save");
    host.File(app, root / "valid-after-bad.txt");
    app.Complete(bad);
    assert(host.work_id && host.booleans["busy"]);
    host.Complete(app);
    assert(Read(root / "valid-after-bad.txt") == "draft");

    app.Edit("edit:0", "retain after rejected work");
    host.reject_submit = true;
    app.Action("close");
    host.Choose(app, 1);
    assert(!host.work_id && !host.task_id && host.booleans["available"]);
    host.reject_task = true;
    app.Action("close");
    assert(host.booleans["available"] && host.strings["text_0"] == "retain after rejected work");
}

void AbiExceptions(const std::filesystem::path &root)
{
    const auto *module = prism_app_module_v1();
    assert(module && module->on_close_request && module->on_task_completed);
    Host host;
    auto api = host.Api();
    notepad::Notepad app(&api);
    app.Edit("edit:0", "retained after exception");

    host.throw_binding = true;
    const PrismCloseRequestV1 first{sizeof(first), 31};
    assert(module->on_close_request(&app, &first) == PRISM_CLOSE_REJECT_V1);
    assert(host.close_results.back() ==
           std::make_pair(std::uint64_t{31}, std::uint32_t{PRISM_CLOSE_REJECT_V1}));
    assert(host.task_id && host.cancelled_tasks.back() == host.task_id);
    // Even a late successful result from the cancelled task cannot discard data.
    host.Choose(app, 2);
    assert(host.booleans["used_0"] && host.strings["text_0"] == "retained after exception");
    assert(host.booleans["available"]);

    host.throw_binding = true;
    module->on_action(&app, {"close", 5});
    assert(host.task_id && host.cancelled_tasks.back() == host.task_id);
    host.Result(app, PRISM_TASK_CANCELLED_V1);
    assert(host.booleans["available"] && host.booleans["used_0"]);

    const PrismCloseRequestV1 second{sizeof(second), 32};
    assert(module->on_close_request(&app, &second) == PRISM_CLOSE_DEFER_V1);
    host.Choose(app, 1);
    assert(host.kind == PRISM_TASK_SAVE_FILE_V1);
    const auto path = (root / "exception-save.txt").string();
    PrismTaskResultV1 result{};
    result.struct_size = sizeof(result);
    result.request_id = host.task_id;
    result.outcome = PRISM_TASK_SUCCEEDED_V1;
    result.file_path = {path.data(), path.size()};
    host.task_id = 0;
    host.throw_submit = true;
    module->on_task_completed(&app, &result);
    assert(!host.work_id && !host.task_id && host.booleans["available"]);
    assert(host.close_results.back() ==
           std::make_pair(std::uint64_t{32}, std::uint32_t{PRISM_CLOSE_REJECT_V1}));
    assert(host.strings["text_0"] == "retained after exception");

    const std::string snapshot(20'000, 's');
    app.Edit("edit:0", snapshot);
    app.Action("save");
    result.request_id = host.task_id;
    host.task_id = 0;
    host.throw_binding = true; // Work was accepted; its owned snapshot must survive recovery.
    module->on_task_completed(&app, &result);
    assert(host.work_id && host.booleans["busy"] && host.booleans["editable"]);
    app.Edit("edit:0", "newer edits after acceptance");
    host.Complete(app);
    assert(Read(path) == snapshot);
    assert(host.strings["text_0"] == "newer edits after acceptance");
    assert(host.strings["details"].find("Unsaved") != std::string::npos);
    assert(host.close_results.size() == 2);

    const PrismCloseRequestV1 third{sizeof(third), 33};
    assert(module->on_close_request(&app, &third) == PRISM_CLOSE_DEFER_V1);
    host.Choose(app, 2);
    assert(host.close_results.back() ==
           std::make_pair(std::uint64_t{33}, std::uint32_t{PRISM_CLOSE_ACCEPT_V1}));
}

void FeedbackSuccessAndRecovery(const std::filesystem::path &root)
{
    Host host;
    auto api = host.Api();
    notepad::Notepad app(&api);
    app.Edit("edit:0", "first receipt");
    app.Action("save");
    host.File(app, root / "feedback-saved.txt");
    assert(!host.feedback_shown); // Selecting a path is not a successful save.
    host.Complete(app);
    assert(host.feedback_kind == PRISM_FEEDBACK_SUCCESS_V1 && host.feedback_duration == 4000);
    assert(host.feedback_title == "Saved" && host.feedback_actions.empty());
    assert(host.feedback_message.find("feedback-saved.txt") != std::string::npos);
    const auto saved_feedback = host.feedback_id;
    app.Edit("edit:0", "second snapshot");
    app.Action("save");
    assert(host.feedback_id == 0 && host.dismissed_feedback.back() == saved_feedback);
    app.Edit("edit:0", "third draft");
    host.Complete(app);
    assert(host.feedback_kind == PRISM_FEEDBACK_SUCCESS_V1 &&
           host.feedback_title == "Snapshot saved");
    assert(host.feedback_message.find("newer edits are still unsaved") != std::string::npos);
    assert(Read(root / "feedback-saved.txt") == "second snapshot");

    app.Action("save-as");
    host.Result(app, PRISM_TASK_CANCELLED_V1);
    assert(host.feedback_kind == PRISM_FEEDBACK_INFO_V1 && host.feedback_duration == 4000);
    assert(host.feedback_actions.empty() && host.strings["text_0"] == "third draft");
    app.Action("save-as");
    const auto failed = root / "missing-feedback" / "retained-name.txt";
    host.File(app, failed);
    host.Complete(app);
    assert(host.feedback_kind == PRISM_FEEDBACK_ERROR_V1 && host.feedback_duration == 0);
    assert(host.feedback_actions.size() == 1 &&
           host.feedback_actions[0].second == "Change location");
    assert(host.feedback_message.find("Resolve file folder") != std::string::npos);
    assert(host.feedback_message.find("retained") != std::string::npos);
    const auto error = host.feedback_id;
    app.Edit("edit:0", "fourth draft after error");
    assert(host.feedback_id == error);
    host.FeedbackAction(app, error, 1);
    assert(host.kind == PRISM_TASK_SAVE_FILE_V1 && host.directory == failed.parent_path().string());
    assert(host.suggested == "retained-name.txt" &&
           host.strings["text_0"] == "fourth draft after error");
    const auto next_task = host.task_id;
    host.FeedbackAction(app, error, 1);
    assert(host.task_id == next_task && !host.work_id);
    host.File(app, root / "feedback-recovered.txt");
    host.Complete(app);
    assert(Read(root / "feedback-recovered.txt") == "fourth draft after error");
    assert(host.feedback_kind == PRISM_FEEDBACK_SUCCESS_V1);
}

void FeedbackPreservesVersions(const std::filesystem::path &root)
{
    Host host;
    auto api = host.Api();
    notepad::Notepad app(&api);
    const auto file = root / "feedback-version.txt";
    std::ofstream(file) << "original";
    app.Action("open");
    host.File(app, file);
    host.Complete(app);
    app.Edit("edit:1", "my changes");
    std::ofstream(file) << "external changes";
    app.Action("save");
    host.Complete(app);
    assert(host.feedback_kind == PRISM_FEEDBACK_ERROR_V1 && host.feedback_actions.size() == 1);
    const auto first = host.feedback_id;
    // No Retry action exists for saving. Even reselecting the same path with
    // fresh Replace intent cannot bypass the loaded document version check.
    host.FeedbackAction(app, first, 1);
    host.File(app, file, true);
    host.Complete(app);
    assert(Read(file) == "external changes" && host.strings["text_1"] == "my changes");
    assert(host.feedback_kind == PRISM_FEEDBACK_ERROR_V1);
    const auto second = host.feedback_id;
    host.FeedbackAction(app, first, 1);
    assert(!host.task_id && !host.work_id && host.feedback_id == second);
    host.FeedbackAction(app, second, 1);
    host.File(app, root / "feedback-version-new.txt");
    host.Complete(app);
    assert(Read(root / "feedback-version-new.txt") == "my changes");
    assert(Read(file) == "external changes");
}

void FeedbackOpenAndIdentity(const std::filesystem::path &root)
{
    Host host;
    auto api = host.Api();
    notepad::Notepad app(&api);
    app.Edit("edit:0", "source draft");
    const auto missing = root / "feedback-open.txt";
    app.Action("open");
    host.File(app, missing);
    host.Complete(app);
    assert(host.feedback_kind == PRISM_FEEDBACK_ERROR_V1 && host.feedback_actions.size() == 2);
    assert(host.feedback_actions[0].second == "Retry" &&
           host.feedback_actions[1].second == "Choose file");
    const auto open_failure = host.feedback_id;
    std::ofstream(missing) << "now available";
    host.FeedbackAction(app, open_failure, 1);
    assert(host.work_id && !host.task_id);
    host.Complete(app);
    assert(host.strings["text_1"] == "now available" && host.strings["text_0"] == "source draft");
    host.FeedbackAction(app, open_failure, 2);
    assert(!host.task_id && !host.work_id);

    app.Action("open");
    host.File(app, root / "another-missing.txt");
    host.Complete(app);
    const auto choose = host.feedback_id;
    host.FeedbackAction(app, choose, 2);
    assert(host.kind == PRISM_TASK_OPEN_FILE_V1 && host.directory == root.string());
    host.Result(app, PRISM_TASK_CANCELLED_V1);

    app.Action("select:0");
    app.Action("save");
    host.File(app, root / "missing-feedback-identity" / "draft.txt");
    host.Complete(app);
    const auto draft_error = host.feedback_id;
    app.Action("select:1");
    assert(host.feedback_id == 0);
    host.FeedbackAction(app, draft_error, 1);
    assert(!host.task_id && !host.work_id && host.booleans["selected_1"]);
    app.Action("select:0");
    host.FeedbackAction(app, draft_error, 1);
    assert(!host.task_id && !host.work_id);
    app.Action("save");
    host.File(app, root / "missing-feedback-close" / "draft.txt");
    host.Complete(app);
    const auto before_close = host.feedback_id;
    assert(Close(app, 41) == PRISM_CLOSE_DEFER_V1);
    const auto close_task = host.task_id;
    host.FeedbackAction(app, before_close, 1);
    assert(host.task_id == close_task && !host.work_id);
    host.Result(app, PRISM_TASK_CANCELLED_V1);
    assert(host.close_results.back().second == PRISM_CLOSE_REJECT_V1);
}

void FeedbackSlotReuse(const std::filesystem::path &root)
{
    Host host;
    auto api = host.Api();
    notepad::Notepad app(&api);
    app.Action("open");
    host.File(app, root / "missing-feedback-reuse.txt");
    host.Complete(app);
    const auto error = host.feedback_id;
    app.Action("close:0");
    assert(host.booleans["used_0"] && host.strings["text_0"].empty());
    host.FeedbackAction(app, error, 1);
    assert(!host.work_id && !host.task_id);
}

void FeedbackFallbackAndBounds(const std::filesystem::path &root)
{
    Host host;
    auto api = host.Api();
    api.struct_size = offsetof(PrismHostApiV1, show_feedback) + sizeof(api.show_feedback) - 1;
    notepad::Notepad app(&api);
    app.Edit("edit:0", "without feedback");
    app.Action("save");
    host.File(app, root / "feedback-short-tail.txt");
    host.Complete(app);
    assert(Read(root / "feedback-short-tail.txt") == "without feedback");
    assert(!host.feedback_queries && !host.feedback_shown &&
           host.strings["status"] == "Saved successfully");
    api.struct_size = sizeof(api);
    host.feedback_capabilities = 0;
    app.Edit("edit:0", "no capability");
    app.Action("save");
    host.Complete(app);
    assert(!host.feedback_shown && Read(root / "feedback-short-tail.txt") == "no capability");
    host.feedback_capabilities = PRISM_FEEDBACK_CAP_OWNER_V1;
    host.throw_feedback = true;
    app.Edit("edit:0", "feedback throw retained success");
    app.Action("save");
    host.Complete(app);
    assert(Read(root / "feedback-short-tail.txt") == "feedback throw retained success");
    assert(host.strings["status"] == "Saved successfully" &&
           Close(app, 42) == PRISM_CLOSE_ACCEPT_V1);

    host.reject_feedback = true;
    app.Edit("edit:0", "feedback rejection retains success");
    app.Action("save");
    host.Complete(app);
    assert(Read(root / "feedback-short-tail.txt") == "feedback rejection retains success");
    assert(host.strings["status"] == "Saved successfully");
    host.reject_feedback = false;

    app.Edit("edit:0", "bounded error draft");
    app.Action("save");
    notepad::FileResult failure;
    failure.error = "reason\twith\rcontrols\xe2\x80\xa8";
    for (unsigned i = 0; i < 800; ++i) {
        failure.error += "你好";
    }
    const auto bytes = nlohmann::json::to_cbor(nlohmann::json(failure));
    PrismWorkCompletionV1 completion{sizeof(completion),           host.work_id,
                                     PRISM_WORK_SUCCEEDED_V1,      0,
                                     {bytes.data(), bytes.size()}, {}};
    host.work_id = 0;
    const auto *module = prism_app_module_v1();
    module->on_work_completed(&app, &completion);
    assert(host.feedback_kind == PRISM_FEEDBACK_ERROR_V1 && host.feedback_message.size() <= 2048);
    assert(runtime::TextBuffer::Valid(host.feedback_message));
    assert(host.feedback_message.find('\t') == std::string::npos &&
           host.feedback_message.find('\r') == std::string::npos);
    assert(host.feedback_message.find("\xe2\x80\xa8") == std::string::npos);
    assert(host.strings["text_0"] == "bounded error draft");
    const auto error = host.feedback_id;
    PrismFeedbackActionEventV1 partial{sizeof(partial) - 1, error, 1};
    module->on_feedback_action(&app, &partial);
    assert(!host.task_id && host.feedback_id == error);
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
    OpenAndSaveAs(temporary);
    OwnerClose(temporary);
    CloseFailures(temporary);
    BadAndStaleResults(temporary);
    AbiExceptions(temporary);
    FeedbackSuccessAndRecovery(temporary);
    FeedbackPreservesVersions(temporary);
    FeedbackOpenAndIdentity(temporary);
    FeedbackSlotReuse(temporary);
    FeedbackFallbackAndBounds(temporary);
    Package(argv[1]);
    std::filesystem::remove_all(temporary);
}
