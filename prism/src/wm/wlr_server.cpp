#include "wlr_server_internal.hpp"

namespace prism::wm {
WlrServer::WlrServer(std::shared_ptr<Compositor> compositor) : compositor_(std::move(compositor))
{
    std::array<std::uint8_t, 8> random{};
    launch::RandomBytes(random);
    for (auto byte : random) {
        control_session_ = (control_session_ << 8) | byte;
    }
    if (!control_session_) {
        throw std::runtime_error("Zero WM session nonce");
    }

    wl_list_init(&signals_.new_surface.link);
    wl_list_init(&signals_.output_layout_change.link);
    wl_list_init(&signals_.new_virtual_keyboard.link);
    wl_list_init(&signals_.new_virtual_pointer.link);
}

WlrServer::~WlrServer()
{
    Stop();
}

bool WlrServer::Initialize(const std::string &socket_name)
{
    PRISM_LOG_INFO("WLR-SERVER",
                   "Initializing Wayland & wlroots Compositor Engine (Sway architecture)...");

    // Enable linear buffer compatibility on Intel ADL-N / i915 DRM planes
    setenv("WLR_DRM_NO_MODIFIERS", "1", 0);

    wlr_log_init(WLR_INFO, NULL);

    // 1. Create Wayland display & event loop
    wl_display_ = wl_display_create();
    if (!wl_display_) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create wl_display");
        return false;
    }
    wl_event_loop_ = wl_display_get_event_loop(wl_display_);

    // 2. Autocreate multi-backend (DRM/KMS, Wayland-nested, or Headless)
    backend_ = wlr_backend_autocreate(wl_event_loop_, nullptr);
    if (!backend_) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create wlr_backend");
        return false;
    }

    // 3. Autocreate hardware renderer & allocator
    renderer_ = wlr_renderer_autocreate(backend_);
    if (!renderer_) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create wlr_renderer");
        return false;
    }
    wlr_renderer_init_wl_shm(renderer_, wl_display_);

    allocator_ = wlr_allocator_autocreate(backend_, renderer_);
    if (!allocator_) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create wlr_allocator");
        return false;
    }

    // 4. Compositor & Subcompositor globals
    wlr_compositor_ = wlr_compositor_create(wl_display_, 5, renderer_);
    signals_.server = this;
    signals_.new_surface.notify = handle_server_new_surface;
    wl_signal_add(&wlr_compositor_->events.new_surface, &signals_.new_surface);
    subcompositor_ = wlr_subcompositor_create(wl_display_);
    if (!wlr_presentation_create(wl_display_, backend_)) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create presentation feedback global");
        return false;
    }
    wlr_data_device_manager_create(wl_display_);
    wlr_screencopy_manager_v1_create(wl_display_);

    // 5. Output layout & Hardware Scene graph
    output_layout_ = wlr_output_layout_create(wl_display_);
    signals_.output_layout_change.notify = HandleOutputLayoutChange;
    wl_signal_add(&output_layout_->events.change, &signals_.output_layout_change);
    wlr_xdg_output_manager_v1_create(wl_display_, output_layout_);
    scene_ = wlr_scene_create();
    wlr_scene_attach_output_layout(scene_, output_layout_);
    if (auto *dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(wl_display_, 4, renderer_)) {
        wlr_scene_set_linux_dmabuf_v1(scene_, dmabuf);
        PRISM_LOG_INFO("WLR-SERVER", "linux-dmabuf-v1 enabled for GPU clients");
    } else {
        PRISM_LOG_INFO("WLR-SERVER", "linux-dmabuf-v1 unavailable for this renderer");
    }

    // 6. Initialize 100% Native GPU Scene-Graph hierarchy
    InitSceneGraph();
    surface_effects_ = std::make_unique<SurfaceEffects>(wl_display_, renderer_, allocator_);
    surface_effects_->SetWakeHandler(std::bind_front(&WlrServer::HandleEffectsWake, this));

    // 7. XDG Shell Protocol (Wayland Window Standard Protocol)
    xdg_shell_ = wlr_xdg_shell_create(wl_display_, 3);
    if (xdg_shell_) {
        signals_.new_xdg_toplevel.notify = handle_server_new_xdg_surface;
        wl_signal_add(&xdg_shell_->events.new_toplevel, &signals_.new_xdg_toplevel);
        PRISM_LOG_INFO("WLR-SERVER",
                       "Wayland XDG-Shell v3 active: Native application windows supported");
    }

    // 8. Seat & Cursor management
    cursor_ = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor_, output_layout_);
    cursor_mgr_ = wlr_xcursor_manager_create(nullptr, 24);
    if (cursor_mgr_) {
        wlr_xcursor_manager_load(cursor_mgr_, 1.0f);
        wlr_cursor_set_xcursor(cursor_, cursor_mgr_, "left_ptr");
    }

    seat_ = wlr_seat_create(wl_display_, "seat0");
    wlr_seat_set_capabilities(seat_, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    virtual_keyboard_manager_ = wlr_virtual_keyboard_manager_v1_create(wl_display_);
    virtual_pointer_manager_ = wlr_virtual_pointer_manager_v1_create(wl_display_);
    if (!virtual_keyboard_manager_ || !virtual_pointer_manager_) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to create virtual input managers");
        return false;
    }
    signals_.new_virtual_keyboard.notify = handle_server_new_virtual_keyboard;
    wl_signal_add(&virtual_keyboard_manager_->events.new_virtual_keyboard,
                  &signals_.new_virtual_keyboard);
    signals_.new_virtual_pointer.notify = handle_server_new_virtual_pointer;
    wl_signal_add(&virtual_pointer_manager_->events.new_virtual_pointer,
                  &signals_.new_virtual_pointer);
    wl_display_set_global_filter(wl_display_, FilterGlobal, this);

    // 9. Wire wlroots signals
    signals_.server = this;
    signals_.new_output.notify = handle_server_new_output;
    wl_signal_add(&backend_->events.new_output, &signals_.new_output);

    signals_.new_input.notify = handle_server_new_input;
    wl_signal_add(&backend_->events.new_input, &signals_.new_input);

    signals_.cursor_motion.notify = handle_cursor_motion;
    wl_signal_add(&cursor_->events.motion, &signals_.cursor_motion);

    signals_.cursor_motion_absolute.notify = handle_cursor_motion_absolute;
    wl_signal_add(&cursor_->events.motion_absolute, &signals_.cursor_motion_absolute);

    signals_.cursor_button.notify = handle_cursor_button;
    wl_signal_add(&cursor_->events.button, &signals_.cursor_button);

    signals_.cursor_axis.notify = handle_cursor_axis;
    wl_signal_add(&cursor_->events.axis, &signals_.cursor_axis);

    // 8. Bind Wayland UNIX domain socket
    const char *sock = nullptr;
    if (!socket_name.empty()) {
        if (wl_display_add_socket(wl_display_, socket_name.c_str()) == 0) {
            sock = socket_name.c_str();
        }
    }
    if (!sock) {
        sock = wl_display_add_socket_auto(wl_display_);
    }
    socket_name_ = sock ? sock : "wayland-prism";

    PRISM_LOG_INFO("WLR-SERVER", "wlroots Compositor initialized successfully on socket '%s'",
                   socket_name_.c_str());

    InitializeIpc();

    last_frame_time_ns_ = core::CurrentTimeNs();
    running_ = true;
    return true;
}

static void auto_create_output_for_nested(struct wlr_backend *backend, void *data)
{
    bool *created = static_cast<bool *>(data);
    if (*created) {
        return;
    }

    PRISM_LOG_INFO("WLR-SERVER", "auto_create callback: backend=%p, is_wl=%d, is_headless=%d",
                   (void *)backend, wlr_backend_is_wl(backend), wlr_backend_is_headless(backend));

    if (wlr_backend_is_wl(backend)) {
        struct wlr_output *out = wlr_wl_output_create(backend);
        PRISM_LOG_INFO("WLR-SERVER", "wlr_wl_output_create returned %p", (void *)out);
        if (out) {
            wlr_wl_output_set_title(out, "Project Prism Desktop");
            *created = true;
            PRISM_LOG_INFO("WLR-SERVER", "Created nested Wayland output window");
        }
    } else if (wlr_backend_is_headless(backend)) {
        struct wlr_output *out = wlr_headless_add_output(backend, 1920, 1080);
        PRISM_LOG_INFO("WLR-SERVER", "wlr_headless_add_output returned %p", (void *)out);
        if (out) {
            *created = true;
            PRISM_LOG_INFO("WLR-SERVER", "Created headless 1920x1080 output");
        }
    }
#if WLR_HAS_X11_BACKEND
    else if (wlr_backend_is_x11(backend)) {
        struct wlr_output *out = wlr_x11_output_create(backend);
        PRISM_LOG_INFO("WLR-SERVER", "wlr_x11_output_create returned %p", (void *)out);
        if (out) {
            wlr_x11_output_set_title(out, "Project Prism Desktop");
            *created = true;
            PRISM_LOG_INFO("WLR-SERVER", "Created nested X11 output window");
        }
    }
#endif
}

void WlrServer::Start()
{
    if (!backend_) {
        return;
    }

    PRISM_LOG_INFO("WLR-SERVER", "Starting wlroots backend...");
    if (!wlr_backend_start(backend_)) {
        PRISM_LOG_ERROR("WLR-SERVER", "Failed to start wlroots backend");
        return;
    }
    PRISM_LOG_INFO("WLR-SERVER", "Backend started! is_multi=%d, is_wl=%d, outputs=%zu",
                   wlr_backend_is_multi(backend_), wlr_backend_is_wl(backend_), outputs_.size());

    // Auto-create output window ONLY if no outputs exist (prevents duplicate output window)
    bool created = false;
    if (outputs_.empty()) {
        if (wlr_backend_is_multi(backend_)) {
            wlr_multi_for_each_backend(backend_, auto_create_output_for_nested, &created);
        } else {
            auto_create_output_for_nested(backend_, &created);
        }
    }

    PRISM_LOG_INFO("WLR-SERVER",
                   "wlroots backend running! Wayland display listening on %s (created_nested=%d)",
                   socket_name_.c_str(), created ? 1 : 0);
}

void WlrServer::Stop()
{
    if (!wl_display_) {
        return;
    }
    running_ = false;
    wl_list_remove(&signals_.output_layout_change.link);
    wl_list_init(&signals_.output_layout_change.link);
    layout_subscribed_ = false;
    layout_snapshot_.reset();
    layout_tree_snapshot_.reset();
    registrations_.clear();
    if (surface_effects_) {
        surface_effects_->SetWakeHandler({});
    }
    if (effects_idle_) {
        wl_event_source_remove(effects_idle_);
        effects_idle_ = nullptr;
    }
    surface_geometry_.reset();
    if (control_fade_) {
        control_fade_->Reset(surface_effects_.get());
        control_fade_.reset();
    }
    if (wl_display_) {
        wl_display_destroy_clients(wl_display_);
    }
    wl_list_remove(&signals_.new_virtual_keyboard.link);
    wl_list_init(&signals_.new_virtual_keyboard.link);
    wl_list_remove(&signals_.new_virtual_pointer.link);
    wl_list_init(&signals_.new_virtual_pointer.link);
    surface_watches_.clear();
    wl_list_remove(&signals_.new_surface.link);
    wl_list_init(&signals_.new_surface.link);
    surface_effects_.reset();

    focused_xdg_view_ = nullptr;
    xdg_views_.clear();
    keyboards_.clear();
    touches_.clear();
    pointers_.clear();
    layout_controls_.Reset();
    control_pointer_device_ = nullptr;
    outputs_.clear();
    dock_icon_rects_.clear();

    drag_manager_.reset();

    if (scene_) {
        wlr_scene_node_destroy(&scene_->tree.node);
        scene_ = nullptr;
    }

    if (cursor_mgr_) {
        wlr_xcursor_manager_destroy(cursor_mgr_);
        cursor_mgr_ = nullptr;
    }
    if (cursor_) {
        wlr_cursor_destroy(cursor_);
        cursor_ = nullptr;
    }
    if (output_layout_) {
        wlr_output_layout_destroy(output_layout_);
        output_layout_ = nullptr;
    }
    if (allocator_) {
        wlr_allocator_destroy(allocator_);
        allocator_ = nullptr;
    }
    if (renderer_) {
        wlr_renderer_destroy(renderer_);
        renderer_ = nullptr;
    }
    if (backend_) {
        wlr_backend_destroy(backend_);
        backend_ = nullptr;
    }
    if (wl_display_) {
        wl_display_destroy(wl_display_);
        wl_display_ = nullptr;
        wl_event_loop_ = nullptr;
        virtual_keyboard_manager_ = nullptr;
        virtual_pointer_manager_ = nullptr;
    }

    if (ipc_server_) {
        ipc_server_->Stop();
        ipc_server_.reset();
    }

    PRISM_LOG_INFO("WLR-SERVER", "wlroots server stopped cleanly");
}

bool WlrServer::FilterGlobal(const wl_client *client, const wl_global *global, void *data)
{
    const auto *server = static_cast<const WlrServer *>(data);
    if (global != server->virtual_keyboard_manager_->global &&
        global != server->virtual_pointer_manager_->global) {
        return true;
    }

    uid_t uid = static_cast<uid_t>(-1);
    wl_client_get_credentials(client, nullptr, &uid, nullptr);
    return uid == geteuid();
}

void WlrServer::RunEventLoopIteration(int timeout_ms)
{
    if (!wl_event_loop_ || !wl_display_) {
        return;
    }
    PumpControl();
    wl_event_loop_dispatch(wl_event_loop_, timeout_ms);
    PumpControl();
    wl_display_flush_clients(wl_display_);
    UpdateGroupRecovery();
    PublishLayoutSnapshot();
}

} // namespace prism::wm
