#include "app_host_p.hpp"
#include "client_application_p.hpp"
#include "owner_task_native_p.hpp"
#include "scene_p.hpp"

#include <tuple>

namespace {
using namespace prism::tests::owner_task_probe;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Snapshot = prism::runtime::InputSnapshot;

struct NativeFocus {
    std::uint64_t seat{};
    prism::contracts::NodeId node{};
    bool operator==(const NativeFocus &) const = default;
};

class Owner {
public:
    Owner(const char *socket, const std::filesystem::path &package,
          const std::filesystem::path &source)
        : host(Config(socket, source))
    {
        const auto loaded = prism::launch::LoadPackage(package);
        app_id = loaded.manifest.app_id;
        Require(host.Bind(loaded), "Tooltip native Notepad Host Bind failed");
    }

    static prism::sdk::HostConfig Config(const char *socket, const std::filesystem::path &source)
    {
        prism::sdk::HostConfig config;
        config.socket = socket;
        config.request = {9201};
        config.instance = {9201};
        config.initial_theme =
            prism::theme::LoadTheme(source / "resources/themes", "square", 1, "light");
        return config;
    }

    prism::sdk::ClientApplication &Frontend()
    {
        Require(bool(host.impl_->frontend), "Tooltip native Host has no frontend");
        return *host.impl_->frontend;
    }

    prism::runtime::Scene &Scene()
    {
        Require(bool(Frontend().impl_->scene), "Tooltip native Host has no Scene");
        return *Frontend().impl_->scene;
    }

    std::string String(std::string_view key) const
    {
        const auto at = host.impl_->bindings.find(std::string(key));
        if (at == host.impl_->bindings.end()) {
            return {};
        }
        const auto *value = std::get_if<std::string>(&at->second);
        Require(value, "Expected string Notepad business binding");
        return *value;
    }

    bool Boolean(std::string_view key) const
    {
        const auto at = host.impl_->bindings.find(std::string(key));
        if (at == host.impl_->bindings.end()) {
            return false;
        }
        const auto *value = std::get_if<bool>(&at->second);
        Require(value, "Expected bool Notepad business binding");
        return *value;
    }

    std::shared_ptr<const Snapshot> Adopted() const
    {
        const auto &app = *host.impl_->frontend->impl_;
        const auto &frame = app.ui_root_metadata_frame;
        if (!frame || frame->ui != app.installed_ui || !frame->input_snapshot || !app.scene ||
            !app.scene->IsInputSnapshotAdopted(*frame->input_snapshot)) {
            return {};
        }
        return frame->input_snapshot;
    }

    std::vector<NativeFocus> FocusSnapshot()
    {
        std::vector<NativeFocus> result;
        for (const auto &focus : Scene().input_state_->focus) {
            if (focus.seat && focus.node && Scene().Find(focus.node)) {
                result.push_back({focus.seat, focus.node});
            }
        }
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
            return std::tie(a.seat, a.node.index, a.node.generation) <
                   std::tie(b.seat, b.node.index, b.node.generation);
        });
        return result;
    }

    bool Focused(std::string_view action, bool keyboard = false)
    {
        for (const auto &focus : Scene().input_state_->focus) {
            const auto *node = Scene().Find(focus.node);
            if (focus.seat && node && node->action == action && (!keyboard || focus.visible)) {
                return true;
            }
        }
        return false;
    }

    void Pump()
    {
        Require(host.Pump(10), "Tooltip native Notepad stopped unexpectedly");
    }

    prism::sdk::AppHost host;
    std::string app_id;
};

template <typename Predicate> void Wait(Owner &owner, Predicate ready, std::string_view detail)
{
    const auto deadline = Clock::now() + 8s;
    while (!ready()) {
        Require(Clock::now() < deadline, detail);
        owner.Pump();
    }
}

Point ActionPoint(Owner &owner, std::string_view action)
{
    const auto deadline = Clock::now() + 8s;
    for (;;) {
        const auto input = owner.Adopted();
        const auto tree = Tree();
        std::optional<Rect> global;
        for (const auto &workspace : tree.workspaces) {
            if (const auto bounds = FindOwner(workspace, owner.app_id)) {
                Require(!global, "Tooltip native app identity matched two views");
                global = bounds;
            }
        }
        if (input && global && std::abs(global->width - input->viewport.width) < 1 &&
            std::abs(global->height - input->viewport.height) < 1) {
            for (const auto &node : input->nodes) {
                if (!node.visible || !node.enabled || !node.interactive || node.action != action) {
                    continue;
                }
                const Point point{node.bounds.x + node.bounds.width / 2,
                                  node.bounds.y + node.bounds.height / 2};
                const auto hit = owner.Scene().HitTest(point, *input);
                if (hit && hit->node == node.id) {
                    return {global->x + point.x, global->y + point.y};
                }
            }
        }
        Require(Clock::now() < deadline,
                std::string("Tooltip native action is not hittable: ") + std::string(action));
        owner.Pump();
    }
}

void TypeAt(Owner &owner, Input &input, std::string_view action, std::string_view text)
{
    input.Click(ActionPoint(owner, action));
    Wait(
        owner, [&owner, action] { return owner.Focused(action); },
        "Tooltip native editor never acquired the real input seat focus");
    input.SelectAll();
    for (const auto character : text) {
        input.Type(character);
        owner.Pump();
    }
}

template <typename Node> bool FrameHasTooltipGlyphs(Owner &owner, const Node &tooltip)
{
    const auto &frame = owner.Frontend().impl_->ui_root_metadata_frame;
    if (!frame || !frame->display_list || tooltip.children.size() != 1) {
        return false;
    }
    const auto &text = *tooltip.children.front();
    if (text.kind != prism::runtime::Kind::Text || text.shaped.glyphs.empty()) {
        return false;
    }
    auto glyphs = text.shaped.glyphs;
    for (auto &glyph : glyphs) {
        glyph.origin.x += text.bounds.x;
        glyph.origin.y += text.bounds.y;
    }
    for (const auto &command : frame->display_list->commands) {
        const auto *run = std::get_if<prism::contracts::DrawGlyphRun>(&command);
        if (run && run->glyphs == glyphs && run->font_size == text.style.font_size &&
            run->color == text.style.foreground) {
            return true;
        }
    }
    return false;
}

template <typename Node> void CheckExcluded(const Snapshot &input, const Node &node)
{
    Require(!input.Find(node.id), "Tooltip or its content entered the input snapshot tree");
    Require(node.action.empty(), "Tooltip content acquired a business action");
    for (const auto &child : node.children) {
        CheckExcluded(input, *child);
    }
}

bool TooltipAdopted(Owner &owner, std::string_view action)
{
    const auto &scene = owner.Scene();
    const auto input = owner.Adopted();
    const auto *anchor = scene.Find(scene.TooltipAnchor());
    const auto *tooltip = scene.Find(scene.TooltipNode());
    return input && anchor && tooltip && anchor->action == action &&
           input->tooltip_anchor == anchor->id && input->tooltip_node == tooltip->id &&
           tooltip->bounds.width > 0 && tooltip->bounds.height > 0 &&
           FrameHasTooltipGlyphs(owner, *tooltip);
}

void CheckTooltip(Owner &owner, std::string_view action)
{
    Wait(
        owner, [&owner, action] { return TooltipAdopted(owner, action); },
        "Tooltip never reached actual root frame and input snapshot adoption");
    const auto input = owner.Adopted();
    const auto *tooltip = owner.Scene().Find(owner.Scene().TooltipNode());
    Require(tooltip && tooltip->bounds.x >= 0 && tooltip->bounds.y >= 0 &&
                tooltip->bounds.x + tooltip->bounds.width <= input->viewport.width &&
                tooltip->bounds.y + tooltip->bounds.height <= input->viewport.height,
            "Tooltip was not clamped to its actual viewport");
    CheckExcluded(*input, *tooltip);
    Require(!owner.Scene().PopupToken() && !owner.Scene().OwnerModalToken(),
            "Tooltip created a Popup or owner-modal input scope");
}

bool TooltipHidden(Owner &owner)
{
    const auto input = owner.Adopted();
    return !owner.Scene().TooltipNode() && input && !input->tooltip_anchor && !input->tooltip_node;
}

void CheckNoReopen(Owner &owner)
{
    // This bounded native check spans the authored hover delay. Exact deadline
    // arithmetic belongs to the deterministic Tooltip core tests.
    const auto deadline = Clock::now() + 650ms;
    do {
        Require(TooltipHidden(owner), "Esc suppression allowed the same Tooltip to reopen");
        owner.Pump();
    } while (Clock::now() < deadline);
}

struct Report {
    bool passed{}, actual_notepad_module{}, native_hover{}, native_keyboard{}, readonly_adoption{},
        viewport_clamped{}, existing_focus_preserved{}, keyboard_without_refocus{},
        escape_suppressed{}, keyboard_immediate{}, native_key_exactly_once{},
        native_click_exactly_once{}, task_precedence{}, draft_retained{};
    std::uint64_t focus_samples{}, adopted_tooltips{}, adopted_tasks{}, pixel_commits{};
    std::string renderer{"V3D"}, theme{"square/light"}, error;
    std::vector<std::string> scenarios;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Report, passed, actual_notepad_module, native_hover,
                                   native_keyboard, readonly_adoption, viewport_clamped,
                                   existing_focus_preserved, keyboard_without_refocus,
                                   escape_suppressed, keyboard_immediate, native_key_exactly_once,
                                   native_click_exactly_once, task_precedence, draft_retained,
                                   focus_samples, adopted_tooltips, adopted_tasks, pixel_commits,
                                   renderer, theme, error, scenarios)

void Run(Owner &owner, Input &input, Report &report)
{
    Wait(
        owner, [&owner] { return owner.host.GetUiState().master_presented && owner.Adopted(); },
        "Tooltip native Notepad Master did not reach actual presentation");
    Require(owner.Frontend().GlRenderer().find("V3D") != std::string::npos,
            "Tooltip native gate requires hardware V3D");
    Require(owner.Frontend().SupportsOwnerFileTasks(), "Notepad shared file task unavailable");
    report.actual_notepad_module = true;

    TypeAt(owner, input, "edit:0", "new");
    Wait(
        owner, [&owner] { return owner.String("text_0") == "new"; },
        "Native text did not reach the actual Notepad module");
    const auto before = owner.FocusSnapshot();
    Require(!before.empty() && owner.Focused("edit:0"),
            "Hover focus proof requires a nonempty native editor focus sample");
    ++report.focus_samples;
    input.Move(ActionPoint(owner, "new"));
    CheckTooltip(owner, "new");
    ++report.adopted_tooltips;
    Require(owner.FocusSnapshot() == before, "Hover Tooltip adoption changed native focus");
    report.existing_focus_preserved = true;
    for (const auto character : std::string_view(".md")) {
        input.Type(character);
        owner.Pump();
    }
    Wait(
        owner, [&owner] { return owner.String("text_0") == "new.md"; },
        "Tooltip prevented native typing without clicking the editor again");
    Require(owner.FocusSnapshot() == before && !owner.Boolean("used_1"),
            "Tooltip changed focus or accidentally invoked the New document action");
    report.native_hover = report.keyboard_without_refocus = true;
    report.scenarios.emplace_back("hover-focus-and-keyboard");

    input.Move(ActionPoint(owner, "edit:0"));
    Wait(owner, [&owner] { return TooltipHidden(owner); }, "Pointer exit did not hide Tooltip");
    input.Move(ActionPoint(owner, "new"));
    CheckTooltip(owner, "new");
    ++report.adopted_tooltips;
    input.Escape();
    Wait(owner, [&owner] { return TooltipHidden(owner); }, "Esc did not hide Tooltip");
    CheckNoReopen(owner);
    report.escape_suppressed = true;
    report.scenarios.emplace_back("escape-suppression");

    input.Move(ActionPoint(owner, "edit:0"));
    for (unsigned count = 0; count < 32 && !owner.Focused("new", true); ++count) {
        input.Key(15); // Native Linux Tab; no Scene focus setter is used.
        owner.Pump();
    }
    Wait(
        owner, [&owner] { return owner.Focused("new", true); },
        "Native Tab never focused the New document target");
    const auto *immediate = owner.Scene().Find(owner.Scene().TooltipNode());
    Require(immediate && immediate->tooltip_for == "new",
            "Keyboard focus used the pointer hover delay for Tooltip");
    CheckTooltip(owner, "new");
    ++report.adopted_tooltips;
    report.keyboard_immediate = true;
    input.Key(28); // Native Linux Enter activates the original focused target.
    Wait(
        owner, [&owner] { return owner.Boolean("used_1"); },
        "Tooltip prevented native keyboard activation of its anchor");
    Require(!owner.Boolean("used_2") && owner.String("text_0") == "new.md",
            "Native Enter invoked New more than once or lost the previous draft");
    report.native_keyboard = report.native_key_exactly_once = true;
    report.scenarios.emplace_back("keyboard-focus-and-activation");

    input.Click(ActionPoint(owner, "edit:1"));
    Wait(
        owner, [&owner] { return owner.Focused("edit:1"); },
        "Second native editor did not acquire focus");
    input.Move(ActionPoint(owner, "new"));
    CheckTooltip(owner, "new");
    ++report.adopted_tooltips;
    input.Click(ActionPoint(owner, "new"));
    Wait(
        owner, [&owner] { return owner.Boolean("used_2"); },
        "Tooltip blocked native pointer activation of its anchor");
    Require(!owner.Boolean("used_3") && owner.String("text_0") == "new.md",
            "Native click invoked New more than once or lost the original draft");
    report.native_click_exactly_once = true;
    report.scenarios.emplace_back("pointer-click-activation");

    input.Click(ActionPoint(owner, "edit:2"));
    input.Move(ActionPoint(owner, "open-panel"));
    CheckTooltip(owner, "open-panel");
    ++report.adopted_tooltips;
    input.Click(ActionPoint(owner, "open-panel"));
    Wait(
        owner,
        [&owner] {
            const auto task = owner.Frontend().ActiveOwnerTask();
            const auto adopted = owner.Adopted();
            return task && task->phase == prism::runtime::TaskPhase::Ready && adopted &&
                   adopted->owner_modal_epoch == owner.Scene().OwnerModalEpoch();
        },
        "Native Open did not reach the shared file task's actual adopted Ready phase");
    Require(owner.Scene().OwnerModalToken() && !owner.Scene().TooltipNode(),
            "Owner file task did not take precedence over Tooltip");
    ++report.adopted_tasks;
    report.task_precedence = true;
    input.Escape();
    Wait(
        owner,
        [&owner] {
            return !owner.Frontend().ActiveOwnerTask() && owner.Adopted() &&
                   !owner.Scene().OwnerModalToken();
        },
        "Cancelling the shared file task did not restore normal Notepad input");
    Require(owner.String("text_0") == "new.md" && owner.Boolean("used_1") &&
                owner.Boolean("used_2") && !owner.Boolean("used_3"),
            "Cancelling Open lost drafts or changed the native activation count");
    report.draft_retained = true;
    report.scenarios.emplace_back("file-task-precedence");

    report.readonly_adoption = report.viewport_clamped = true;
    report.pixel_commits = owner.Frontend().GetPlatformStatus().surface_pixel_commits;
    report.passed = true;
    owner.host.Close();
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 5) {
        return 2;
    }
    Report report;
    try {
        Input input;
        input.Connect(argv[1]);
        Owner owner(argv[1], argv[2], argv[3]);
        Run(owner, input, report);
    } catch (const std::exception &error) {
        report.error = error.what();
        std::cerr << "Tooltip native probe failed: " << error.what() << '\n';
    }
    std::ofstream(argv[4]) << nlohmann::json(report).dump(2) << '\n';
    return report.passed ? 0 : 1;
}
