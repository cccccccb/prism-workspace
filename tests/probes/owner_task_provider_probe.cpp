#include "../fixtures/owner_task_fixture.h"
#include "app_host_p.hpp"
#include "client_application_p.hpp"
#include "owner_task_native_p.hpp"
#include "prism/ipc/wm_messages.hpp"
#include "prism/runtime/owner_file_panel.hpp"
#include "prism/runtime/owner_task_panel.hpp"
#include "scene_p.hpp"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

namespace {
using namespace prism::tests::owner_task_probe;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Host = prism::sdk::AppHost;
using Point = prism::contracts::LogicalPoint;
using Rect = prism::contracts::LogicalRect;
using Snapshot = prism::runtime::InputSnapshot;
using prism::runtime::TaskIdentity;

class Fixture {
public:
    explicit Fixture(const std::filesystem::path &module)
    {
        handle_ = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
        Require(handle_, "Cannot load native task fixture exports");
        host = Symbol<decltype(host)>("owner_task_provider_host");
        actions = Symbol<decltype(actions)>("owner_task_provider_actions");
        count = Symbol<decltype(count)>("owner_task_provider_count");
        record = Symbol<decltype(record)>("owner_task_provider_record");
        reenter = Symbol<decltype(reenter)>("owner_task_provider_reenter");
        next_request = Symbol<decltype(next_request)>("owner_task_provider_next_request");
        errors = Symbol<decltype(errors)>("owner_task_provider_errors");
    }

    ~Fixture()
    {
        if (handle_) {
            dlclose(handle_);
        }
    }

    const PrismHostApiV1 *(*host)(){};
    std::size_t (*actions)(){};
    std::size_t (*count)(){};
    const OwnerTaskFixtureRecord *(*record)(std::size_t){};
    void (*reenter)(std::uint32_t){};
    std::uint64_t (*next_request)(){};
    unsigned (*errors)(){};

private:
    template <typename Function> Function Symbol(const char *name)
    {
        const auto result = reinterpret_cast<Function>(dlsym(handle_, name));
        Require(result, std::string("Missing native fixture export: ") + name);
        return result;
    }

    void *handle_{};
};

prism::sdk::HostConfig Config(const char *socket, const std::filesystem::path &source,
                              std::uint64_t instance)
{
    prism::sdk::HostConfig config;
    config.socket = socket;
    config.request = {instance};
    config.instance = {instance};
    config.initial_theme =
        prism::theme::LoadTheme(source / "resources/themes", "square", 1, "light");
    return config;
}

class Owner {
public:
    Owner(const char *socket, const std::filesystem::path &package,
          const std::filesystem::path &source, std::uint64_t instance)
        : fixture(package / "business.so"), host(Config(socket, source, instance))
    {
        const auto loaded = prism::launch::LoadPackage(package);
        app_id = loaded.manifest.app_id;
        Require(host.Bind(loaded), "Native task Host Bind failed");
    }

    void Pump()
    {
        if (!host.Pump(10)) {
            Dump();
            Require(false, "Native owner-task Host stopped unexpectedly");
        }
        Require(fixture.errors() == 0, "Native task fixture rejected a callback/action");
    }

    prism::sdk::ClientApplication &Frontend()
    {
        Require(bool(host.impl_->frontend), "Native Host has no frontend");
        return *host.impl_->frontend;
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

    bool Ready() const
    {
        const auto active = host.impl_->frontend->ActiveOwnerTask();
        const auto &app = *host.impl_->frontend->impl_;
        const auto input = Adopted();
        return active && active->phase == prism::runtime::TaskPhase::Ready &&
               app.owner_task_scope && app.owner_task_scope->identity == active->identity &&
               input && input->owner_modal_epoch && app.owner_task_scope->ui == app.installed_ui &&
               app.owner_task_scope->token == app.scene->OwnerModalToken();
    }

    std::uint64_t Request()
    {
        const auto *api = fixture.host();
        Require(api && (api->task_capabilities(api->context) & PRISM_TASK_CAP_CONFIRMATION_V1),
                "Native Host provider capability is unavailable");
        const std::array choices{
            PrismTaskChoiceV1{
                sizeof(PrismTaskChoiceV1), 11, {"Discard", 7}, PRISM_TASK_CHOICE_DESTRUCTIVE_V1},
            PrismTaskChoiceV1{
                sizeof(PrismTaskChoiceV1), 22, {"Save", 4}, PRISM_TASK_CHOICE_PRIMARY_V1}};
        std::string title = "Keep these changes?";
        std::string message = "This document has unsaved changes. Choose how to continue.";
        const PrismTaskRequestV1 request{sizeof(request),
                                         PRISM_TASK_CONFIRMATION_V1,
                                         {title.data(), title.size()},
                                         {message.data(), message.size()},
                                         choices.data(),
                                         choices.size(),
                                         nullptr};
        const auto count = fixture.count();
        const auto id = api->request_task(api->context, &request);
        Require(id && fixture.count() == count && !Frontend().ActiveOwnerTask(),
                "Task request was rejected or synchronously presented/completed");
        title.assign(title.size(), 'X');
        message.assign(message.size(), 'Y');
        Require(host.impl_->owner_task &&
                    host.impl_->owner_task->request.title == "Keep these changes?",
                "Native provider retained borrowed request text");
        return id;
    }

    std::uint64_t FileRequest(std::uint32_t kind, const std::filesystem::path &directory,
                              std::string name = {})
    {
        const auto *api = fixture.host();
        const auto capability = prism::contracts::OwnerTaskCapability(
            static_cast<prism::contracts::OwnerTaskKind>(kind));
        Require(api && capability && (api->task_capabilities(api->context) & capability),
                "Native file provider capability is unavailable");
        std::string path = directory.string();
        std::string extension = ".md";
        const PrismTaskStringViewV1 filter{extension.data(), extension.size()};
        const PrismFileTaskOptionsV1 options{sizeof(options),
                                             {path.data(), path.size()},
                                             {name.data(), name.size()},
                                             kind == PRISM_TASK_SELECT_DIRECTORY_V1 ? nullptr
                                                                                    : &filter,
                                             kind == PRISM_TASK_SELECT_DIRECTORY_V1 ? 0u : 1u,
                                             0};
        const PrismTaskRequestV1 request{sizeof(request),
                                         kind,
                                         {"Choose a document", 17},
                                         {"Continue in this window.", 24},
                                         nullptr,
                                         0,
                                         &options};
        const auto before = fixture.count();
        const auto id = api->request_task(api->context, &request);
        Require(id && fixture.count() == before && !Frontend().ActiveOwnerTask(),
                "File request rejected or presented/completed before returning");
        path.assign(path.size(), 'X');
        name.assign(name.size(), 'Y');
        extension.assign(extension.size(), 'Z');
        Require(host.impl_->owner_task && host.impl_->owner_task->request.file &&
                    host.impl_->owner_task->request.file->initial_directory == directory.string(),
                "Native file provider retained a borrowed directory hint");
        return id;
    }

    void Dump() const
    {
        const auto ui = host.GetUiState();
        std::cerr << "native task diagnostic app=" << app_id << " failed=" << ui.failed
                  << " installed=" << ui.master_installed << " presented=" << ui.master_presented
                  << " backend_ready=" << host.impl_->ready << " actions=" << fixture.actions()
                  << " completions=" << fixture.count();
        if (host.impl_->frontend) {
            const auto active = host.impl_->frontend->ActiveOwnerTask();
            const auto status = host.impl_->frontend->GetPlatformStatus();
            const auto input = Adopted();
            std::cerr << " pixels=" << status.surface_pixel_commits << " adopted=" << bool(input);
            if (active) {
                std::cerr << " owner=" << active->identity.owner.value
                          << " request=" << active->identity.request.value
                          << " phase=" << static_cast<int>(active->phase);
            }
            if (input) {
                std::cerr << " epoch=" << input->owner_modal_epoch
                          << " input_version=" << input->version;
            }
            if (const auto *scene = host.impl_->frontend->impl_->scene.get()) {
                for (const auto &focus : scene->input_state_->focus) {
                    const auto *node = scene->Find(focus.node);
                    std::cerr << " focus[seat=" << focus.seat << ",node=" << focus.node.index
                              << ",action=" << (node ? node->action : "missing") << ']';
                }
                for (const auto &pointer : scene->input_state_->pointers) {
                    const auto *node = scene->Find(pointer.hovered);
                    std::cerr << " pointer[seat=" << pointer.source.seat
                              << ",device=" << pointer.source.device
                              << ",generation=" << pointer.source.generation
                              << ",inside=" << pointer.inside << ",x=" << pointer.position.x
                              << ",y=" << pointer.position.y
                              << ",hover=" << (node ? node->action : "none") << ']';
                }
            }
        }
        std::cerr << '\n';
    }

    // The fixture handle outlives Host shutdown; diagnostic records remain safe
    // to inspect after Close, but the borrowed Host API must not be called then.
    Fixture fixture;
    Host host;
    std::string app_id;
};

class Group {
public:
    Group(Owner &first, Owner *second = nullptr) : owners_{&first, second}
    {
    }

    void Pump()
    {
        for (auto *owner : owners_) {
            if (owner) {
                owner->Pump();
            }
        }
    }

    void CheckDeadline(Clock::time_point deadline, std::string_view detail)
    {
        if (Clock::now() >= deadline) {
            for (auto *owner : owners_) {
                if (owner) {
                    owner->Dump();
                }
            }
            Require(false, detail);
        }
    }

private:
    std::array<Owner *, 2> owners_;
};

void Started(Group &group, Owner &owner)
{
    const auto deadline = Clock::now() + 8s;
    while (!owner.host.GetUiState().master_presented || !owner.host.impl_->ready ||
           !owner.Adopted()) {
        group.CheckDeadline(deadline, "Master never reached actual native presentation/adoption");
        group.Pump();
    }
    Require(owner.Frontend().GlRenderer().find("V3D") != std::string::npos,
            "Owner-task native gate requires hardware V3D rendering");
    Require(owner.Frontend().SupportsOwnerConfirmation(),
            "Native Master did not receive the shared task panel");
}

void Ready(Group &group, Owner &owner)
{
    const auto deadline = Clock::now() + 5s;
    while (!owner.Ready()) {
        group.CheckDeadline(deadline, "Task never adopted real worker input geometry as Ready");
        group.Pump();
    }
}

void Results(Group &group, Owner &owner, std::size_t expected)
{
    const auto deadline = Clock::now() + 5s;
    while (owner.fixture.count() < expected) {
        group.CheckDeadline(deadline, "Expected native task terminal never reached the module");
        group.Pump();
    }
    Require(owner.fixture.count() == expected && owner.fixture.errors() == 0,
            "Native task terminal had extra or malformed callbacks");
}

void Actions(Group &group, Owner &owner, std::size_t expected)
{
    const auto deadline = Clock::now() + 5s;
    while (owner.fixture.actions() < expected) {
        group.CheckDeadline(deadline, "Expected ordinary owner action never reached the module");
        group.Pump();
    }
    Require(owner.fixture.actions() == expected, "Ordinary owner action duplicated");
}

void Restored(Group &group, Owner &owner)
{
    const auto deadline = Clock::now() + 5s;
    while (owner.Frontend().ActiveOwnerTask() || !owner.Adopted() ||
           owner.host.impl_->frontend->impl_->scene->OwnerModalToken()) {
        group.CheckDeadline(deadline, "Owner input did not adopt the restored modal boundary");
        group.Pump();
    }
}

Point ActionPoint(Group &group, Owner &owner, std::string_view action)
{
    const auto deadline = Clock::now() + 5s;
    for (;;) {
        const auto input = owner.Adopted();
        const auto tree = Tree();
        std::optional<Rect> global;
        for (const auto &workspace : tree.workspaces) {
            if (const auto bounds = FindOwner(workspace, owner.app_id)) {
                Require(!global, "Native app identity unexpectedly matched two views");
                global = bounds;
            }
        }
        if (input && global && std::abs(global->width - input->viewport.width) < 1 &&
            std::abs(global->height - input->viewport.height) < 1) {
            const prism::runtime::InputSnapshotNode *target = nullptr;
            for (const auto &node : input->nodes) {
                if (node.id && node.visible && node.enabled && node.action == action) {
                    Require(!target, "Native action unexpectedly has two visible targets");
                    target = &node;
                }
            }
            if (target) {
                Require(target->bounds.width >= 32 && target->bounds.height >= 32,
                        "Native action target violates its minimum hit size");
                const Point point{target->bounds.x + target->bounds.width / 2,
                                  target->bounds.y + target->bounds.height / 2};
                const auto hit = owner.Frontend().impl_->scene->HitTest(point, *input);
                if (target->interactive && (!hit || hit->node != target->id)) {
                    std::cerr << "native action geometry action=" << action
                              << " target=" << target->id.index << " x=" << point.x
                              << " y=" << point.y << " hit=" << (hit ? hit->node.index : UINT32_MAX)
                              << '\n';
                    owner.Dump();
                    Require(false, "Native action center is clipped or covered in its snapshot");
                }
                return {global->x + point.x, global->y + point.y};
            }
        }
        group.CheckDeadline(deadline,
                            "Native action never matched committed view/adopted geometry");
        group.Pump();
    }
}

struct Adoption {
    std::uint64_t owner{}, request{}, epoch{}, input_version{}, frame_sequence{}, pixel_commits{};
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Adoption, owner, request, epoch, input_version, frame_sequence,
                                   pixel_commits)

Adoption Observe(Owner &owner)
{
    Require(owner.Ready(), "Native task observation preceded Ready/input adoption");
    const auto active = *owner.Frontend().ActiveOwnerTask();
    const auto input = owner.Adopted();
    const auto &frame = owner.Frontend().impl_->ui_root_metadata_frame;
    return {active.identity.owner.value,
            active.identity.request.value,
            input->owner_modal_epoch,
            input->version,
            frame->sequence,
            owner.Frontend().GetPlatformStatus().surface_pixel_commits};
}

void Terminal(Owner &owner, std::size_t index, std::uint64_t request, std::uint32_t outcome,
              std::uint32_t choice = 0, std::uint32_t cancellation = 0)
{
    const auto *result = owner.fixture.record(index);
    Require(result && result->request_id == request && result->outcome == outcome &&
                result->choice_id == choice && result->cancel_reason == cancellation &&
                !result->failure_code && !result->diagnostic_size,
            "Native module received an incorrect typed terminal or correlation");
}

struct CaseReport {
    std::string name;
    bool passed{}, real_pointer{}, real_keyboard{}, owner_isolated{};
    std::vector<Adoption> adopted;
    std::uint64_t callbacks{}, ordinary_actions{};
    bool filesystem_no_write{}, overwrite_revalidated{}, stale_input_rejected{}, file_fd_drained{};
    bool complete_path_caption{}, busy_hints_checked{}, pure_host_projection{};
    std::vector<std::string> file_paths;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CaseReport, name, passed, real_pointer, real_keyboard,
                                   owner_isolated, adopted, callbacks, ordinary_actions,
                                   filesystem_no_write, overwrite_revalidated, stale_input_rejected,
                                   file_fd_drained, complete_path_caption, busy_hints_checked,
                                   pure_host_projection, file_paths)

CaseReport Single(const char *socket, const std::filesystem::path &packages,
                  const std::filesystem::path &source, Input &input, std::string name,
                  std::uint64_t instance)
{
    Owner owner(socket, packages / name, source, instance);
    Group group(owner);
    Started(group, owner);
    input.Click(ActionPoint(group, owner, "owner-action"));
    Actions(group, owner, 1);
    const auto outside = ActionPoint(group, owner, "owner-action");
    const auto request = owner.Request();
    CaseReport report;
    report.name = name;
    if (name == "business-cancel") {
        const auto *api = owner.fixture.host();
        Require(api->cancel_task(api->context, request) == 0 && owner.fixture.count() == 0,
                "Business cancellation rejected or synchronously called its module");
        Results(group, owner, 1);
        Terminal(owner, 0, request, PRISM_TASK_CANCELLED_V1, 0, PRISM_TASK_CANCEL_USER_V1);

        Restored(group, owner);
        const auto cancel_first = owner.Request();
        Require(cancel_first > request, "Business cancellation reused its request correlation");
        Ready(group, owner);
        report.adopted.push_back(Observe(owner));
        const auto cancel_identity = owner.Frontend().ActiveOwnerTask()->identity;
        Require(api->cancel_task(api->context, cancel_first) == 0 && owner.fixture.count() == 1,
                "Ready cancellation rejected or called the module before returning");

        // Simulate a same-turn provider/theme failure after the Host accepted
        // cancellation. It must not replace the already accepted User intent.
        Require(owner.Frontend().FailOwnerTask(cancel_identity,
                                               {prism::runtime::TaskFailureCode::PreparationFailed,
                                                "Provider failed after cancellation"}) &&
                    owner.fixture.count() == 1,
                "Late provider failure was rejected or synchronously delivered a terminal");
        Results(group, owner, 2);
        Terminal(owner, 1, cancel_first, PRISM_TASK_CANCELLED_V1, 0, PRISM_TASK_CANCEL_USER_V1);

        // Reverse the ordering: a core terminal accepted before cancel_task
        // makes cancellation unavailable, preserving the original failure.
        Restored(group, owner);
        const auto failure_first = owner.Request();
        Require(failure_first > cancel_first, "Later failure reused a cancelled correlation");
        Ready(group, owner);
        report.adopted.push_back(Observe(owner));
        const auto failure_identity = owner.Frontend().ActiveOwnerTask()->identity;
        constexpr std::string_view diagnostic = "Provider failed before cancellation";
        Require(owner.Frontend().FailOwnerTask(failure_identity,
                                               {prism::runtime::TaskFailureCode::PreparationFailed,
                                                std::string(diagnostic)}) &&
                    owner.fixture.count() == 2,
                "Earlier provider terminal was rejected or synchronously delivered");
        Require(api->cancel_task(api->context, failure_first) == -1 && owner.fixture.count() == 2,
                "Cancellation rewrote an accepted terminal or synchronously called its module");
        Results(group, owner, 3);
        const auto *failure = owner.fixture.record(2);
        Require(failure && failure->request_id == failure_first &&
                    failure->outcome == PRISM_TASK_FAILED_V1 && !failure->choice_id &&
                    !failure->cancel_reason &&
                    failure->failure_code == PRISM_TASK_PREPARATION_FAILED_V1 &&
                    failure->diagnostic_size == diagnostic.size() &&
                    std::string_view(failure->diagnostic, failure->diagnostic_size) == diagnostic,
                "Rejected cancellation altered the preceding typed failure");
    } else {
        Ready(group, owner);
        report.adopted.push_back(Observe(owner));
        // This pointer press/release is serialized before the actual task
        // terminal input. The positive terminal fence proves it was processed.
        input.Click(outside);
        if (name == "owner-close") {
            owner.host.Close();
            Require(owner.fixture.count() == 0 && !owner.host.Pump(0),
                    "Owner shutdown delivered a task terminal or remained pumpable");
            report.passed = report.owner_isolated = true;
            report.ordinary_actions = owner.fixture.actions();
            Require(report.ordinary_actions == 1, "Owner modal leaked its outside input");
            return report;
        }

        if (name == "escape") {
            input.Escape();
            report.real_keyboard = true;
        } else {
            if (name == "callback-next") {
                owner.fixture.reenter(1);
            }
            const auto action = name == "cancel" ? prism::runtime::kOwnerTaskCancelAction
                                                 : prism::runtime::kOwnerTaskChoiceActions[1];
            input.Click(ActionPoint(group, owner, action));
            report.real_pointer = true;
        }
        Results(group, owner, 1);
        if (name == "cancel" || name == "escape") {
            Terminal(owner, 0, request, PRISM_TASK_CANCELLED_V1, 0,
                     name == "escape" ? PRISM_TASK_CANCEL_ESCAPE_V1 : PRISM_TASK_CANCEL_USER_V1);
        } else {
            Terminal(owner, 0, request, PRISM_TASK_SUCCEEDED_V1, 22);
        }
        Require(owner.fixture.actions() == 1, "Owner modal leaked its outside pointer click");

        if (name == "callback-next") {
            const auto next = owner.fixture.next_request();
            Require(next > request && owner.host.impl_->owner_task &&
                        owner.host.impl_->owner_task->request.request_id == next,
                    "Completion callback did not stage a distinct next request");
            Ready(group, owner);
            report.adopted.push_back(Observe(owner));
            Require(report.adopted[1].owner == report.adopted[0].owner &&
                        report.adopted[1].request > report.adopted[0].request &&
                        report.adopted[1].epoch > report.adopted[0].epoch,
                    "Reentrant task reused its scope/request or changed owner lifetime");
            const auto *api = owner.fixture.host();
            Require(api->cancel_task(api->context, request) == -1,
                    "Old correlation cancelled the new callback task");
            input.Click(ActionPoint(group, owner, prism::runtime::kOwnerTaskChoiceActions[0]));
            Results(group, owner, 2);
            Terminal(owner, 1, next, PRISM_TASK_SUCCEEDED_V1, 7);
        }
    }

    Restored(group, owner);
    input.Click(ActionPoint(group, owner, "owner-action"));
    Actions(group, owner, 2);
    const auto expected_callbacks = name == "business-cancel" ? 3u
                                    : name == "callback-next" ? 2u
                                                              : 1u;
    Require(owner.fixture.count() == expected_callbacks,
            "Restoring ordinary input duplicated a task callback");
    report.callbacks = owner.fixture.count();
    report.ordinary_actions = owner.fixture.actions();
    report.passed = report.owner_isolated = true;
    owner.host.Close();
    return report;
}

CaseReport TwoOwners(const char *socket, const std::filesystem::path &packages,
                     const std::filesystem::path &source, Input &input)
{
    Owner first(socket, packages / "owner-one", source, 107);
    Owner second(socket, packages / "owner-two", source, 108);
    Group group(first, &second);
    Started(group, first);
    Started(group, second);
    input.Click(ActionPoint(group, second, "owner-action"));
    Actions(group, second, 1);
    const auto one = first.Request();
    Ready(group, first);
    const auto first_identity = first.Frontend().ActiveOwnerTask()->identity;
    input.Click(ActionPoint(group, second, "owner-action"));
    Actions(group, second, 2);
    Require(first.Ready() && first.fixture.count() == 0,
            "Other owner's ordinary input cancelled or completed the first task");
    const auto two = second.Request();
    Ready(group, second);
    const auto second_identity = second.Frontend().ActiveOwnerTask()->identity;
    Require(first_identity.owner != second_identity.owner &&
                !second.Frontend().CompleteOwnerTask(first_identity) &&
                !first.Frontend().CancelOwnerTask(second_identity),
            "Foreign SDK owner identity crossed the real Host binding");

    CaseReport report;
    report.name = "two-owners";
    report.adopted = {Observe(first), Observe(second)};
    input.Click(ActionPoint(group, first, prism::runtime::kOwnerTaskChoiceActions[0]));
    Results(group, first, 1);
    Terminal(first, 0, one, PRISM_TASK_SUCCEEDED_V1, 11);
    Require(second.fixture.count() == 0 && second.Ready(),
            "First owner's actual choice affected the other task");
    input.Click(ActionPoint(group, second, "owner-action"));
    input.Escape();
    Results(group, second, 1);
    Terminal(second, 0, two, PRISM_TASK_CANCELLED_V1, 0, PRISM_TASK_CANCEL_ESCAPE_V1);
    Require(second.fixture.actions() == 2 && first.fixture.count() == 1,
            "Second owner's modal scope leaked or callbacks crossed owners");
    Restored(group, first);
    Restored(group, second);
    report.callbacks = first.fixture.count() + second.fixture.count();
    report.ordinary_actions = second.fixture.actions();
    report.passed = report.real_pointer = report.real_keyboard = report.owner_isolated = true;
    return report;
}

using FilePhase = prism::sdk::OwnerFileState::Phase;

void FileState(Group &group, Owner &owner, FilePhase phase,
               const std::filesystem::path &directory = {})
{
    const auto deadline = Clock::now() + 5s;
    for (;;) {
        const auto &task = owner.host.impl_->owner_task;
        if (task && task->file && task->file->phase == phase && owner.Ready() &&
            (directory.empty() || task->file->directory == directory.string())) {
            return;
        }
        group.CheckDeadline(deadline, "File task did not adopt its expected semantic projection");
        group.Pump();
    }
}

void FileSelection(Group &group, Owner &owner, std::string_view name, TaskIdentity identity,
                   std::uint64_t preceding_epoch)
{
    const auto deadline = Clock::now() + 5s;
    for (;;) {
        const auto &task = owner.host.impl_->owner_task;
        const auto active = owner.Frontend().ActiveOwnerTask();
        const auto input = owner.Adopted();
        if (task && task->file && task->file->phase == FilePhase::Browsing &&
            task->file->selected && task->file->listing && task->file->listing->directory &&
            active && active->identity == identity && owner.Ready() && input &&
            input->owner_modal_epoch > preceding_epoch) {
            const auto &file = *task->file;
            const auto index = *file.selected;
            const auto &entries = file.listing->directory->entries;
            if (index < entries.size() && entries[index].name == name) {
                return;
            }
        }
        group.CheckDeadline(deadline, "Native row selection lacks its matching adoption receipt");
        group.Pump();
    }
}

void FullCaption(Owner &owner, const std::filesystem::path &path)
{
    const auto view = owner.host.impl_->FilePanelView();
    Require(prism::contracts::ValidOwnerFilePath(path.string()) &&
                view.selected_caption == path.string(),
            "File detail projection lost its complete canonical selection path");
}

class FileProjectionRestore {
public:
    explicit FileProjectionRestore(prism::sdk::OwnerFileState &file) : file_(file), saved_(file)
    {
    }

    ~FileProjectionRestore()
    {
        file_ = std::move(saved_);
    }

private:
    prism::sdk::OwnerFileState &file_;
    prism::sdk::OwnerFileState saved_;
};

void BusyHints(Owner &owner)
{
    auto &file = *owner.host.impl_->owner_task->file;
    const FileProjectionRestore restore(file);
    // Pure Host presentation contract: no event dispatch, Pump or renderer
    // submission occurs while these synthetic operation phases are inspected.
    const std::array phases{FilePhase::Listing, FilePhase::Validating, FilePhase::Revalidating};
    const std::array<std::string_view, 3> captions{"Loading folder...", "Checking selection...",
                                                   "Checking destination..."};
    for (std::size_t index = 0; index < phases.size(); ++index) {
        if (phases[index] == FilePhase::Revalidating &&
            owner.host.impl_->owner_task->request.kind !=
                prism::contracts::OwnerTaskKind::SaveFile) {
            continue;
        }
        file.phase = phases[index];
        file.status.clear();
        const auto view = owner.host.impl_->FilePanelView();
        Require(view.loading && !view.nav_enabled && !view.submit_enabled &&
                    view.status == captions[index],
                "Busy file projection lacks its disabled controls or operation status");
    }
}

std::string_view RowAction(Owner &owner, std::string_view name)
{
    const auto &file = *owner.host.impl_->owner_task->file;
    Require(file.listing && file.listing->directory, "Native file listing is unavailable");
    for (std::size_t position = 0; position < file.items.size(); ++position) {
        const auto &entry = file.listing->directory->entries[file.items[position]];
        if (entry.name == name) {
            Require(position < prism::runtime::kOwnerFileRowActions.size(),
                    "Native file fixture unexpectedly requires another page");
            return prism::runtime::kOwnerFileRowActions[position];
        }
    }
    Require(false, "Native file listing lost the expected fixture item");
    return {};
}

void OldSnapshotClick(Owner &owner, const std::shared_ptr<const Snapshot> &snapshot,
                      std::string_view action)
{
    const prism::runtime::InputSnapshotNode *target{};
    for (const auto &node : snapshot->nodes) {
        if (node.visible && node.enabled && node.action == action) {
            Require(!target, "Old snapshot action has two visible targets");
            target = &node;
        }
    }
    Require(target, "Old descriptor has no requested target");
    const Point point{target->bounds.x + target->bounds.width / 2,
                      target->bounds.y + target->bounds.height / 2};
    prism::contracts::PointerButtonEvent event{{1},
                                               point,
                                               prism::contracts::PointerButton::Primary,
                                               prism::contracts::ButtonState::Pressed,
                                               0,
                                               1,
                                               {0, 998, 1},
                                               0};
    owner.Frontend().impl_->HandleWindowEvent(event, snapshot);
    event.state = prism::contracts::ButtonState::Released;
    owner.Frontend().impl_->HandleWindowEvent(event, snapshot);
}

bool FileFdReadable(Owner &owner)
{
    Require(bool(owner.host.impl_->file_model), "Native Host never created a file model");
    pollfd descriptor{owner.host.impl_->file_model->Fd(), POLLIN, 0};
    const auto result = poll(&descriptor, 1, 0);
    Require(result >= 0, "Native file-model descriptor poll failed");
    return descriptor.revents & POLLIN;
}

void FileDrained(Group &group, Owner &owner)
{
    const auto deadline = Clock::now() + 5s;
    unsigned quiet{};
    while (quiet < 8) {
        group.CheckDeadline(deadline, "Retired file model retained completion/capacity wakeups");
        group.Pump();
        quiet = !owner.host.impl_->file_model->Pending() && !FileFdReadable(owner) ? quiet + 1 : 0;
    }
}

std::string ReadFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    Require(bool(input), "Native file fixture cannot be read");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void FileTerminal(Owner &owner, std::uint64_t request, const std::filesystem::path &path,
                  bool overwrite = false)
{
    Terminal(owner, 0, request, PRISM_TASK_SUCCEEDED_V1);
    const auto *result = owner.fixture.record(0);
    Require(result &&
                std::string_view(result->file_path, result->file_path_size) == path.string() &&
                result->overwrite_approved == static_cast<std::uint32_t>(overwrite),
            "Native file result lost its canonical path or overwrite decision");
}

bool NativeEditorFocused(const prism::runtime::Scene &scene, prism::contracts::NodeId editor,
                         prism::contracts::InputSource pointer)
{
    if (!editor || !pointer.seat || !pointer.device || !pointer.generation ||
        !scene.State(editor).focused) {
        return false;
    }
    return std::any_of(scene.input_state_->focus.begin(), scene.input_state_->focus.end(),
                       [editor, pointer](const auto &focus) {
                           return focus.seat == pointer.seat && focus.node == editor;
                       });
}

std::optional<prism::contracts::InputSource> NativeEditorPointer(const prism::runtime::Scene &scene,
                                                                 prism::contracts::NodeId editor)
{
    std::optional<prism::contracts::InputSource> source;
    for (const auto &pointer : scene.input_state_->pointers) {
        if (pointer.inside && pointer.hovered == editor && pointer.source.seat &&
            pointer.source.device && pointer.source.generation) {
            Require(!source, "Native filename matched two active pointer sources");
            source = pointer.source;
        }
    }
    return source;
}

void FilenameDiagnostic(Owner &owner, prism::contracts::NodeId editor, std::string_view expected)
{
    const auto &app = *owner.Frontend().impl_;
    const auto *node = app.scene->Find(editor);
    std::cerr << "native filename diagnostic expected=\"" << expected << '"';
    if (const auto &task = owner.host.impl_->owner_task; task && task->file) {
        std::cerr << " host=\"" << task->file->filename
                  << "\" file_phase=" << static_cast<int>(task->file->phase) << " status=\""
                  << task->file->status << '"';
    }
    if (app.owner_file_view) {
        std::cerr << " projection=\"" << app.owner_file_view->filename << '"';
    }
    if (node) {
        std::cerr << " editor=\"" << node->editor.Text() << "\" node_text=\"" << node->text
                  << "\" cursor=" << node->editor.Cursor() << " anchor=" << node->editor.Anchor()
                  << " focused=" << app.scene->State(editor).focused;
    }
    std::cerr << " processed_window_sequence=" << app.last_processed_window_sequence << '\n';
}

void NativeFilename(Group &group, Owner &owner, Input &input, std::string_view name)
{
    const auto identity = owner.Frontend().ActiveOwnerTask()->identity;
    input.Click(ActionPoint(group, owner, prism::runtime::kOwnerFileNameAction));
    const auto focused_deadline = Clock::now() + 5s;
    prism::contracts::NodeId editor;
    prism::contracts::InputSource pointer_source;
    for (;;) {
        const auto active = owner.Frontend().ActiveOwnerTask();
        const auto snapshot = owner.Adopted();
        const auto &scene = *owner.Frontend().impl_->scene;
        if (active && active->identity == identity && owner.Ready() && snapshot) {
            for (const auto &node : snapshot->nodes) {
                if (node.visible && node.enabled &&
                    node.action == prism::runtime::kOwnerFileNameAction) {
                    const auto source = NativeEditorPointer(scene, node.id);
                    if (source && NativeEditorFocused(scene, node.id, *source)) {
                        Require(!editor, "Native filename focus matched two visible editors");
                        editor = node.id;
                        pointer_source = *source;
                    }
                }
            }
            if (editor) {
                break;
            }
        }
        group.CheckDeadline(focused_deadline, "Native filename click never focused its editor");
        group.Pump();
    }

    input.SelectAll();
    const auto selection_deadline = Clock::now() + 5s;
    for (;;) {
        const auto active = owner.Frontend().ActiveOwnerTask();
        const auto &scene = *owner.Frontend().impl_->scene;
        const auto *node = scene.Find(editor);
        if (active && active->identity == identity && owner.Ready() && node &&
            NativeEditorFocused(scene, editor, pointer_source) &&
            node->action == prism::runtime::kOwnerFileNameAction &&
            std::min(node->editor.Cursor(), node->editor.Anchor()) == 0 &&
            std::max(node->editor.Cursor(), node->editor.Anchor()) == node->editor.Text().size()) {
            break;
        }
        group.CheckDeadline(selection_deadline, "Native Ctrl+A never selected the filename text");
        group.Pump();
    }

    std::string expected;
    for (const auto character : name) {
        input.Type(character);
        expected += character;
        const auto deadline = Clock::now() + 5s;
        while (!owner.host.impl_->owner_task || !owner.host.impl_->owner_task->file ||
               owner.host.impl_->owner_task->file->filename != expected || !owner.Ready() ||
               owner.Frontend().ActiveOwnerTask()->identity != identity ||
               !NativeEditorFocused(*owner.Frontend().impl_->scene, editor, pointer_source)) {
            if (Clock::now() >= deadline) {
                FilenameDiagnostic(owner, editor, expected);
            }
            group.CheckDeadline(deadline, "Native keyboard edit did not adopt its filename value");
            group.Pump();
        }
    }
}

CaseReport FileCase(const char *socket, const std::filesystem::path &packages,
                    const std::filesystem::path &source, Input &input, std::string name,
                    std::uint64_t instance)
{
    Owner owner(socket, packages / name, source, instance);
    Group group(owner);
    Started(group, owner);
    input.Click(ActionPoint(group, owner, "owner-action"));
    Actions(group, owner, 1);
    const auto outside = ActionPoint(group, owner, "owner-action");
    const auto directory =
        std::filesystem::canonical(packages.parent_path() / "files" / name / "input");
    const auto existing = directory / "existing.md";
    const auto original = ReadFile(existing);
    const auto original_open = ReadFile(directory / "old.md");
    const auto original_nested = ReadFile(directory / "folder" / "inner.md");
    const bool save = name.starts_with("file-save-");
    const bool selecting_directory = name == "file-directory";
    const auto kind = save                  ? PRISM_TASK_SAVE_FILE_V1
                      : selecting_directory ? PRISM_TASK_SELECT_DIRECTORY_V1
                                            : PRISM_TASK_OPEN_FILE_V1;
    const auto request = owner.FileRequest(
        kind, directory, save ? name == "file-save-new" ? "draft.md" : "existing.md" : "");
    FileState(group, owner, FilePhase::Browsing, directory);
    CaseReport report;
    report.name = name;
    report.adopted.push_back(Observe(owner));
    const auto identity = owner.Frontend().ActiveOwnerTask()->identity;
    FullCaption(owner, save ? directory / owner.host.impl_->owner_task->file->filename : directory);
    BusyHints(owner);
    report.complete_path_caption = report.busy_hints_checked = report.pure_host_projection = true;
    input.Click(outside);

    if (name == "file-owner-close") {
        input.Click(ActionPoint(group, owner, RowAction(owner, "folder")));
        group.Pump();
        owner.host.Close();
        Require(owner.fixture.count() == 0 && !owner.host.Pump(0) &&
                    !owner.host.impl_->file_model->Pending(),
                "Closing a file owner delivered a callback or left its worker active");
        Require(!FileFdReadable(owner), "Closed file model retained a readable completion FD");
        report.passed = report.real_pointer = report.owner_isolated = report.file_fd_drained = true;
        report.filesystem_no_write = ReadFile(existing) == original &&
                                     ReadFile(directory / "old.md") == original_open &&
                                     ReadFile(directory / "folder" / "inner.md") == original_nested;
        report.ordinary_actions = owner.fixture.actions();
        Require(report.filesystem_no_write && report.ordinary_actions == 1,
                "Closed file owner modified a file or leaked outside input");
        return report;
    }

    if (name == "file-cancel-stale") {
        const auto stale = owner.Adopted();
        const auto folder_action = RowAction(owner, "folder");
        input.Click(ActionPoint(group, owner, folder_action));
        FileState(group, owner, FilePhase::Browsing, directory / "folder");
        FullCaption(owner, directory / "folder");
        report.adopted.push_back(Observe(owner));
        Require(owner.Frontend().ActiveOwnerTask()->identity == identity &&
                    owner.Adopted()->owner_modal_epoch > stale->owner_modal_epoch,
                "Native navigation reused an old input epoch or replaced the task identity");
        OldSnapshotClick(owner, stale, folder_action);
        group.Pump();
        Require(!owner.host.impl_->owner_task->file->selected && owner.fixture.count() == 0,
                "Old directory-row descriptor selected the replacement row");
        input.Click(ActionPoint(group, owner, prism::runtime::kOwnerTaskCancelAction));
        Results(group, owner, 1);
        Terminal(owner, 0, request, PRISM_TASK_CANCELLED_V1, 0, PRISM_TASK_CANCEL_USER_V1);
        Restored(group, owner);
        FileDrained(group, owner);

        const auto next = owner.FileRequest(PRISM_TASK_OPEN_FILE_V1, directory);
        FileState(group, owner, FilePhase::Browsing, directory);
        report.adopted.push_back(Observe(owner));
        OldSnapshotClick(owner, stale, prism::runtime::kOwnerTaskCancelAction);
        group.Pump();
        Require(owner.fixture.count() == 1 && owner.Ready() &&
                    owner.host.impl_->owner_task->request.request_id == next,
                "Old file cancellation descriptor ended a later task");
        input.Escape();
        Results(group, owner, 2);
        Terminal(owner, 1, next, PRISM_TASK_CANCELLED_V1, 0, PRISM_TASK_CANCEL_ESCAPE_V1);
        report.real_keyboard = report.stale_input_rejected = true;
    } else {
        std::filesystem::path result;
        if (name == "file-open") {
            const auto preceding_epoch = owner.Adopted()->owner_modal_epoch;
            input.Click(ActionPoint(group, owner, RowAction(owner, "old.md")));
            FileSelection(group, owner, "old.md", identity, preceding_epoch);
            report.adopted.push_back(Observe(owner));
            result = directory / "old.md";
            FullCaption(owner, result);
        } else if (name == "file-directory") {
            input.Click(ActionPoint(group, owner, RowAction(owner, "folder")));
            FileState(group, owner, FilePhase::Browsing, directory / "folder");
            report.adopted.push_back(Observe(owner));
            result = directory / "folder";
            FullCaption(owner, result);
        } else if (name == "file-save-new") {
            NativeFilename(group, owner, input, "new.md");
            report.real_keyboard = true;
            report.adopted.push_back(Observe(owner));
            result = directory / "new.md";
            FullCaption(owner, result);
            Require(!std::filesystem::exists(result), "New destination unexpectedly exists");
        } else {
            result = existing;
        }

        input.Click(ActionPoint(group, owner, prism::runtime::kOwnerFileSubmitAction));
        const bool overwrite = name == "file-save-overwrite" || name == "file-save-changed";
        if (overwrite) {
            FileState(group, owner, FilePhase::Overwrite, directory);
            Require(owner.fixture.count() == 0 &&
                        owner.Frontend().ActiveOwnerTask()->identity == identity,
                    "Overwrite confirmation completed or replaced its file task");
            report.adopted.push_back(Observe(owner));
            FullCaption(owner, existing);
            if (name == "file-save-changed") {
                const auto stamp = owner.host.impl_->owner_task->file->overwrite->target_stamp;
                {
                    std::ofstream changed(existing, std::ios::binary | std::ios::trunc);
                    changed << "Externally changed destination with a different size.\n";
                    Require(bool(changed), "Cannot mutate the isolated overwrite fixture");
                }
                input.Click(ActionPoint(group, owner, prism::runtime::kOwnerFileReplaceAction));
                FileState(group, owner, FilePhase::Browsing, directory);
                Require(owner.fixture.count() == 0 &&
                            owner.Frontend().ActiveOwnerTask()->identity == identity &&
                            owner.host.impl_->owner_task->file->status ==
                                "The destination changed. Review it before saving.",
                        "Changed destination bypassed review or delivered success");
                report.adopted.push_back(Observe(owner));
                FullCaption(owner, existing);
                input.Click(ActionPoint(group, owner, prism::runtime::kOwnerFileSubmitAction));
                FileState(group, owner, FilePhase::Overwrite, directory);
                Require(owner.host.impl_->owner_task->file->overwrite->target_stamp != stamp,
                        "Repeated overwrite review retained the original file stamp");
                report.adopted.push_back(Observe(owner));
                FullCaption(owner, existing);
            }
            input.Click(ActionPoint(group, owner, prism::runtime::kOwnerFileReplaceAction));
            report.overwrite_revalidated = true;
        }
        Results(group, owner, 1);
        FileTerminal(owner, request, result, overwrite);
        report.file_paths.push_back(result.string());
        if (name == "file-save-new") {
            Require(!std::filesystem::exists(result), "File service wrote a SaveFile destination");
        }
    }

    Restored(group, owner);
    FileDrained(group, owner);
    report.file_fd_drained = true;
    input.Click(ActionPoint(group, owner, "owner-action"));
    Actions(group, owner, 2);
    report.filesystem_no_write =
        ReadFile(existing) == (name == "file-save-changed"
                                   ? "Externally changed destination with a different size.\n"
                                   : original) &&
        ReadFile(directory / "old.md") == original_open &&
        ReadFile(directory / "folder" / "inner.md") == original_nested;
    Require(report.filesystem_no_write, "File service changed the isolated destination bytes");
    report.callbacks = owner.fixture.count();
    report.ordinary_actions = owner.fixture.actions();
    report.passed = report.real_pointer = report.owner_isolated = true;
    owner.host.Close();
    return report;
}

struct Report {
    unsigned schema_version{1};
    std::string gate{"native-owner-task-provider"};
    bool passed{};
    std::string error;
    std::string scope{"AppHost/C module/shared DSL panel/real V3D worker adoption/native input"};
    std::string limitation{
        "No throughput benchmark, touch input or visual approval; stale descriptors are injected "
        "through the SDK event gate after real V3D adoption; busy hints are a pure Host projection "
        "contract check without submitting synthetic phases"};
    std::vector<CaseReport> scenarios;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Report, schema_version, gate, passed, error, scope, limitation,
                                   scenarios)

void Write(const char *path, const Report &report)
{
    std::ofstream output(path);
    Require(bool(output), "Cannot write owner-task native evidence");
    output << nlohmann::json(report).dump(2) << '\n';
}
} // namespace

int main(int argc, char **argv)
{
    Report report;
    try {
        Require(argc == 5,
                "Usage: owner_task_provider_probe <socket> <packages> <source-root> <report.json>");
        Input input;
        input.Connect(argv[1]);
        const std::filesystem::path packages(argv[2]);
        const std::filesystem::path source(argv[3]);
        std::uint64_t instance = 100;
        for (const auto name :
             {"choice", "cancel", "escape", "business-cancel", "callback-next", "owner-close"}) {
            report.scenarios.push_back(Single(argv[1], packages, source, input, name, ++instance));
            Write(argv[4], report);
        }
        report.scenarios.push_back(TwoOwners(argv[1], packages, source, input));
        instance = 108;
        for (const auto name :
             {"file-open", "file-save-new", "file-save-overwrite", "file-save-changed",
              "file-directory", "file-cancel-stale", "file-owner-close"}) {
            report.scenarios.push_back(
                FileCase(argv[1], packages, source, input, name, ++instance));
            Write(argv[4], report);
        }
        report.passed = true;
        Write(argv[4], report);
        std::cout << "Native owner-task provider: 14 scenarios passed\n";
        return 0;
    } catch (const std::exception &error) {
        report.error = error.what();
        if (argc == 5) {
            Write(argv[4], report);
        }
        std::cerr << "owner_task_provider_probe: " << error.what() << '\n';
        return 1;
    }
}
