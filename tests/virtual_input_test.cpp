#include "fixtures/wm_theme_fixture.hpp"
#include "prism/platform/wayland_window.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/wlr_server.hpp"

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

extern "C" {
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
}

namespace {

using namespace std::chrono_literals;

struct ClientState {
    std::string socket;
    std::atomic<bool> keymap_ready{false};
    std::atomic<bool> keymap_checked{false};
    std::atomic<bool> custom_keymap{false};
    std::atomic<int> capability_phase{0};
    std::atomic<bool> done{false};
    std::atomic<bool> passed{false};
};

struct WindowEvents {
    int scroll_count{};
    double last_scroll{};
    int enter_count{};
    int leave_count{};
    int cancel_count{};
    int focus_lost_count{};
    int shift_tab_count{};
    int plain_tab_count{};
    bool sources_valid{true};
    prism::contracts::InputSource pointer_source{};
    prism::contracts::InputSource cancelled_source{};

    void CheckSource(prism::contracts::InputSource source)
    {
        sources_valid &= source.seat != 0 && source.device != 0 && source.generation != 0;
    }

    void Handle(const prism::contracts::WindowEvent &event)
    {
        if (const auto *scroll = std::get_if<prism::contracts::PointerScrollEvent>(&event)) {
            CheckSource(scroll->source);
            ++scroll_count;
            last_scroll = scroll->delta_y;
        } else if (const auto *enter = std::get_if<prism::contracts::PointerEnterEvent>(&event)) {
            CheckSource(enter->source);
            pointer_source = enter->source;
            ++enter_count;
        } else if (const auto *leave = std::get_if<prism::contracts::PointerLeaveEvent>(&event)) {
            CheckSource(leave->source);
            sources_valid &= leave->source == pointer_source;
            ++leave_count;
        } else if (const auto *cancel = std::get_if<prism::contracts::PointerCancelEvent>(&event)) {
            CheckSource(cancel->source);
            cancelled_source = cancel->source;
            ++cancel_count;
        } else if (const auto *motion = std::get_if<prism::contracts::PointerMotionEvent>(&event)) {
            CheckSource(motion->source);
            sources_valid &= motion->source == pointer_source;
            sources_valid &= motion->position.x >= 0 && motion->position.y >= 0;
        } else if (const auto *button = std::get_if<prism::contracts::PointerButtonEvent>(&event)) {
            CheckSource(button->source);
            sources_valid &= button->source == pointer_source;
        } else if (const auto *focus = std::get_if<prism::contracts::FocusEvent>(&event)) {
            CheckSource(focus->source);
            focus_lost_count += !focus->focused;
        } else if (const auto *key = std::get_if<prism::contracts::KeyEvent>(&event)) {
            CheckSource(key->source);
            if (key->physical_key == 0x2b && key->state == prism::contracts::ButtonState::Pressed) {
                shift_tab_count += key->modifiers.shift;
                plain_tab_count += !key->modifiers.shift;
            }
        }
    }
};

class RemoteClient {
public:
    ~RemoteClient()
    {
        if (keyboard_) {
            zwp_virtual_keyboard_v1_destroy(keyboard_);
        }
        if (pointer_) {
            zwlr_virtual_pointer_v1_destroy(pointer_);
        }
        if (keyboard_manager_) {
            zwp_virtual_keyboard_manager_v1_destroy(keyboard_manager_);
        }
        if (pointer_manager_) {
            zwlr_virtual_pointer_manager_v1_destroy(pointer_manager_);
        }
        if (output_) {
            wl_output_destroy(output_);
        }
        if (seat_) {
            wl_seat_destroy(seat_);
        }
        if (registry_) {
            wl_registry_destroy(registry_);
        }
        if (display_) {
            wl_display_disconnect(display_);
        }
    }

    bool Connect(const std::string &socket)
    {
        display_ = wl_display_connect(socket.c_str());
        if (!display_) {
            return false;
        }
        registry_ = wl_display_get_registry(display_);
        if (!registry_ || wl_registry_add_listener(registry_, &registry_listener_, this) != 0 ||
            wl_display_roundtrip(display_) < 0) {
            return false;
        }

        if (!seat_ || !pointer_manager_ || !keyboard_manager_) {
            return false;
        }
        if (pointer_manager_version_ >= 2 && output_) {
            pointer_ = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
                pointer_manager_, seat_, output_);
        } else {
            pointer_ =
                zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager_, seat_);
        }
        keyboard_ =
            zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager_, seat_);
        return pointer_ && keyboard_;
    }

    bool SendGermanKeymap()
    {
        xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!context) {
            return false;
        }
        const xkb_rule_names names{.rules = nullptr,
                                   .model = nullptr,
                                   .layout = "de",
                                   .variant = nullptr,
                                   .options = nullptr};
        xkb_keymap *keymap =
            xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        char *text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
        if (keymap) {
            const auto shift = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
            shift_mask_ = shift < 32 ? 1u << shift : 0;
            xkb_keymap_unref(keymap);
        }
        xkb_context_unref(context);
        if (!text || !shift_mask_) {
            std::free(text);
            return false;
        }

        const std::size_t length = std::strlen(text) + 1;
        const int fd = memfd_create("prism-test-keymap", MFD_CLOEXEC);
        const bool written = fd >= 0 && WriteAll(fd, text, length);
        std::free(text);
        if (!written) {
            if (fd >= 0) {
                close(fd);
            }
            return false;
        }

        zwp_virtual_keyboard_v1_keymap(keyboard_, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                       static_cast<uint32_t>(length));
        const bool sent = wl_display_roundtrip(display_) >= 0;
        close(fd);
        return sent;
    }

    bool SendInput()
    {
        zwlr_virtual_pointer_v1_motion_absolute(pointer_, 100, 500, 500, 1000, 1000);
        zwlr_virtual_pointer_v1_frame(pointer_);
        zwlr_virtual_pointer_v1_button(pointer_, 101, 272, WL_POINTER_BUTTON_STATE_PRESSED);
        zwlr_virtual_pointer_v1_frame(pointer_);
        zwlr_virtual_pointer_v1_button(pointer_, 102, 272, WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(pointer_);
        zwlr_virtual_pointer_v1_axis_source(pointer_, WL_POINTER_AXIS_SOURCE_WHEEL);
        zwlr_virtual_pointer_v1_axis_discrete(pointer_, 103, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                              wl_fixed_from_double(15.0), 1);
        zwlr_virtual_pointer_v1_frame(pointer_);
        zwp_virtual_keyboard_v1_key(keyboard_, 103, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(keyboard_, 104, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
        zwp_virtual_keyboard_v1_modifiers(keyboard_, shift_mask_, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(keyboard_, 105, 15, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(keyboard_, 106, 15, WL_KEYBOARD_KEY_STATE_RELEASED);
        zwp_virtual_keyboard_v1_modifiers(keyboard_, 0, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(keyboard_, 107, 15, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(keyboard_, 108, 15, WL_KEYBOARD_KEY_STATE_RELEASED);
        return wl_display_roundtrip(display_) >= 0;
    }

    bool CloseDevices()
    {
        zwp_virtual_keyboard_v1_destroy(keyboard_);
        keyboard_ = nullptr;
        zwlr_virtual_pointer_v1_destroy(pointer_);
        pointer_ = nullptr;
        return wl_display_roundtrip(display_) >= 0;
    }

private:
    static bool WriteAll(int fd, const char *data, std::size_t length)
    {
        std::size_t offset = 0;
        while (offset < length) {
            const ssize_t count = write(fd, data + offset, length - offset);
            if (count <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    static void OnGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface,
                         uint32_t version)
    {
        auto *client = static_cast<RemoteClient *>(data);
        if (std::strcmp(interface, wl_seat_interface.name) == 0 && !client->seat_) {
            client->seat_ = static_cast<wl_seat *>(
                wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 7u)));
        } else if (std::strcmp(interface, wl_output_interface.name) == 0 && !client->output_) {
            client->output_ =
                static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, 1));
        } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
            client->pointer_manager_version_ = std::min(version, 2u);
            client->pointer_manager_ = static_cast<zwlr_virtual_pointer_manager_v1 *>(
                wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface,
                                 client->pointer_manager_version_));
        } else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
            client->keyboard_manager_ = static_cast<zwp_virtual_keyboard_manager_v1 *>(
                wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
        }
    }

    static void OnGlobalRemove(void *, wl_registry *, uint32_t)
    {
    }

    static constexpr wl_registry_listener registry_listener_{OnGlobal, OnGlobalRemove};
    wl_display *display_{};
    wl_registry *registry_{};
    wl_seat *seat_{};
    wl_output *output_{};
    zwlr_virtual_pointer_manager_v1 *pointer_manager_{};
    zwp_virtual_keyboard_manager_v1 *keyboard_manager_{};
    zwlr_virtual_pointer_v1 *pointer_{};
    zwp_virtual_keyboard_v1 *keyboard_{};
    uint32_t pointer_manager_version_{};
    uint32_t shift_mask_{};
};

void Paint(void *data, int width, int height, int stride)
{
    auto *pixels = static_cast<std::uint32_t *>(data);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pixels[y * (stride / 4) + x] = 0x00336699;
        }
    }
}

bool RunClient(ClientState &state)
{
    prism::platform::WaylandWindow window;
    WindowEvents events;
    window.SetPaintHandler(Paint);
    window.SetEventHandler(std::bind_front(&WindowEvents::Handle, &events));
    if (!window.Open(state.socket, "prism.virtual-input-test", "Virtual Input Test", 640, 400)) {
        return false;
    }
    const auto map_deadline = std::chrono::steady_clock::now() + 5s;
    while ((!window.IsMapped() || window.FrameDoneCount() == 0) &&
           std::chrono::steady_clock::now() < map_deadline) {
        if (!window.Pump(20)) {
            return false;
        }
    }
    if (!window.IsMapped() || window.FrameDoneCount() == 0) {
        return false;
    }

    RemoteClient remote;
    if (!remote.Connect(state.socket) || !remote.SendGermanKeymap()) {
        return false;
    }
    state.keymap_ready = true;
    const auto keymap_deadline = std::chrono::steady_clock::now() + 3s;
    while (!state.keymap_checked && std::chrono::steady_clock::now() < keymap_deadline) {
        std::this_thread::sleep_for(2ms);
    }
    if (!state.keymap_checked || !state.custom_keymap || !remote.SendInput()) {
        return false;
    }

    const auto input_deadline = std::chrono::steady_clock::now() + 4s;
    while ((window.PointerButtonCount() < 2 || window.KeyCount() < 6 || events.scroll_count < 1) &&
           std::chrono::steady_clock::now() < input_deadline) {
        if (!window.Pump(20)) {
            return false;
        }
    }
    const bool delivered = window.PointerEnterCount() > 0 && events.enter_count > 0 &&
                           window.PointerButtonCount() >= 2 && window.KeyCount() >= 6 &&
                           events.scroll_count == 1 && events.last_scroll == 15.0 &&
                           events.shift_tab_count == 1 && events.plain_tab_count == 1 &&
                           events.sources_valid;
    if (!delivered) {
        return false;
    }

    const auto original_source = events.pointer_source;
    const int focus_lost_before = events.focus_lost_count;
    state.capability_phase = 1;
    const auto removal_deadline = std::chrono::steady_clock::now() + 3s;
    while ((events.cancel_count == 0 || events.leave_count == 0 ||
            events.focus_lost_count == focus_lost_before) &&
           std::chrono::steady_clock::now() < removal_deadline) {
        if (!window.Pump(20)) {
            return false;
        }
    }
    if (events.cancel_count != 1 || events.leave_count == 0 ||
        events.focus_lost_count == focus_lost_before ||
        events.cancelled_source != original_source) {
        return false;
    }

    state.capability_phase = 3;
    const auto restore_deadline = std::chrono::steady_clock::now() + 3s;
    while (state.capability_phase != 4 && std::chrono::steady_clock::now() < restore_deadline) {
        if (!window.Pump(20)) {
            return false;
        }
    }
    // Roundtrip the window's get_pointer/get_keyboard requests before sending
    // the next input sequence from the independent virtual-device connection.
    if (state.capability_phase != 4 || wl_display_roundtrip(window.Display()) < 0 ||
        wl_display_roundtrip(window.Display()) < 0 || !remote.SendInput()) {
        return false;
    }
    while ((events.scroll_count < 2 || events.plain_tab_count < 2) &&
           std::chrono::steady_clock::now() < restore_deadline) {
        if (!window.Pump(20)) {
            return false;
        }
    }
    const bool restored_source = events.sources_valid && events.scroll_count == 2 &&
                                 events.shift_tab_count == 2 && events.plain_tab_count == 2 &&
                                 events.pointer_source.seat == original_source.seat &&
                                 events.pointer_source.device == original_source.device &&
                                 events.pointer_source.generation > original_source.generation;
    return restored_source && remote.CloseDevices();
}

void ClientThread(ClientState *state)
{
    state->passed = RunClient(*state);
    state->done = true;
}

bool HasGermanYMapping(prism::wm::WlrServer &server)
{
    auto *keyboard = wlr_seat_get_keyboard(server.GetSeat());
    if (!keyboard || !keyboard->keymap) {
        return false;
    }
    const xkb_keysym_t *symbols = nullptr;
    const int count = xkb_keymap_key_get_syms_by_level(keyboard->keymap, 21 + 8, 0, 0, &symbols);
    return count == 1 && symbols[0] == XKB_KEY_z;
}

} // namespace

int main()
{
    char runtime_template[] = "/tmp/prism-virtual-input-test.XXXXXX";
    char *runtime_dir = mkdtemp(runtime_template);
    if (!runtime_dir) {
        return 1;
    }
    setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("XKB_DEFAULT_LAYOUT", "us", 1);

    auto compositor = std::make_shared<prism::wm::Compositor>();
    if (!compositor->Initialize()) {
        std::filesystem::remove_all(runtime_dir);
        return 2;
    }
    prism::wm::WlrServer server(compositor);
    const std::string socket = "wayland-prism-virtual-input-" + std::to_string(getpid());
    if (!server.Initialize(socket)) {
        std::filesystem::remove_all(runtime_dir);
        return 3;
    }
    server.Start();
    if (!server.InstallTheme(prism::test::WmThemeFixture()).success) {
        server.Stop();
        std::filesystem::remove_all(runtime_dir);
        return 4;
    }

    wlr_keyboard physical_keyboard{};
    static const wlr_keyboard_impl keyboard_impl{.name = "prism-test-physical-keyboard",
                                                 .led_update = nullptr};
    wlr_keyboard_init(&physical_keyboard, &keyboard_impl, "prism-test-physical-keyboard");
    server.HandleNewInput(&physical_keyboard.base);

    ClientState state{.socket = socket};
    std::thread client(ClientThread, &state);
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!state.done && std::chrono::steady_clock::now() < deadline) {
        server.RunEventLoopIteration(10);
        if (state.keymap_ready && !state.keymap_checked) {
            state.custom_keymap = HasGermanYMapping(server);
            state.keymap_checked = true;
        }
        if (state.capability_phase == 1) {
            wlr_seat_pointer_notify_clear_focus(server.GetSeat());
            wlr_seat_pointer_notify_frame(server.GetSeat());
            wlr_seat_set_capabilities(server.GetSeat(), 0);
            state.capability_phase = 2;
        } else if (state.capability_phase == 3) {
            wlr_seat_set_capabilities(server.GetSeat(),
                                      WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
            state.capability_phase = 4;
        }
    }
    const bool timed_out = !state.done;
    if (timed_out) {
        server.Stop();
    }
    client.join();
    const bool restored =
        !timed_out && wlr_seat_get_keyboard(server.GetSeat()) == &physical_keyboard;
    const bool passed = state.passed && state.custom_keymap && restored;
    std::fprintf(stderr, "virtual-input: delivered=%d custom-keymap=%d physical-restored=%d\n",
                 state.passed.load(), state.custom_keymap.load(), restored);

    wlr_keyboard_finish(&physical_keyboard);
    server.Stop();
    std::filesystem::remove_all(runtime_dir);
    return passed ? 0 : 5;
}
