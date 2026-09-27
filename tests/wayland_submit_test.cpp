// Protocol transaction fixture only. It intentionally does not render pixels;
// real GPU rendering is exercised by skia_gles_wayland_probe.
#include "presentation-time-client-protocol.h"
#include "prism/platform/wayland_window.hpp"
#include "xdg-shell-protocol.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-server.h>

namespace {
using namespace std::chrono_literals;
using prism::platform::SubmitRequest;
using prism::platform::SubmitResult;

struct Snapshot {
    unsigned commits{}, frames{}, feedbacks{}, live_frames{}, live_feedbacks{}, pending_frames{},
        pending_feedbacks{}, surfaces{};
};

class Server {
    struct Surface;

    struct Record {
        Surface *surface;
        wl_resource *resource;
        bool feedback, pending{true};
    };

    struct Surface {
        Server *server;
        wl_resource *resource;
        wl_resource *xdg{};
        wl_resource *toplevel{};
        bool configured{};
        std::vector<Record *> records;
    };

    wl_display *display_{};
    wl_global *compositor_{};
    wl_global *shell_{};
    wl_global *presentation_{};
    std::thread thread_;
    std::atomic<bool> stop_{};
    std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
    Snapshot totals_;
    Surface *current_{};

    void ReleaseFramesOnThread()
    {
        assert(current_);
        const auto records = current_->records;
        for (auto *record : records) {
            if (!record->pending && !record->feedback) {
                wl_callback_send_done(record->resource, 1);
                wl_resource_destroy(record->resource);
            }
        }
    }

    void ReleaseFeedbackOnThread(unsigned index, bool discarded)
    {
        assert(current_);
        unsigned current = 0;
        for (auto *record : current_->records) {
            if (record->pending || !record->feedback || current++ != index) {
                continue;
            }
            if (discarded) {
                wl_resource_post_event(record->resource, 2);
            } else {
                wl_resource_post_event(record->resource, 1, 0u, 1u, 0u, 16666667u, 0u, 1u, 0u);
            }
            wl_resource_destroy(record->resource);
            return;
        }
        assert(false);
    }

    static void Destroy(wl_client *, wl_resource *resource)
    {
        wl_resource_destroy(resource);
    }

    static void DestroyRecord(wl_resource *resource)
    {
        auto *record = static_cast<Record *>(wl_resource_get_user_data(resource));
        std::erase(record->surface->records, record);
        delete record;
    }

    static void DestroySurface(wl_resource *resource)
    {
        auto *surface = static_cast<Surface *>(wl_resource_get_user_data(resource));
        while (!surface->records.empty()) {
            wl_resource_destroy(surface->records.back()->resource);
        }
        if (surface->xdg) {
            wl_resource_set_user_data(surface->xdg, nullptr);
        }
        if (surface->toplevel) {
            wl_resource_set_user_data(surface->toplevel, nullptr);
        }
        if (surface->server->current_ == surface) {
            surface->server->current_ = nullptr;
        }
        --surface->server->totals_.surfaces;
        delete surface;
    }

    static void DestroyXdg(wl_resource *resource)
    {
        if (auto *surface = static_cast<Surface *>(wl_resource_get_user_data(resource))) {
            surface->xdg = nullptr;
        }
    }

    static void DestroyToplevel(wl_resource *resource)
    {
        if (auto *surface = static_cast<Surface *>(wl_resource_get_user_data(resource))) {
            surface->toplevel = nullptr;
        }
    }

    static void Configure(Surface &surface, int width = 160, int height = 90)
    {
        assert(surface.xdg && surface.toplevel);
        wl_array states;
        wl_array_init(&states);
        xdg_toplevel_send_configure(surface.toplevel, width, height, &states);
        wl_array_release(&states);
        xdg_surface_send_configure(surface.xdg, wl_display_next_serial(surface.server->display_));
        surface.configured = true;
    }

    static void Commit(wl_client *, wl_resource *resource)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        ++surface.server->totals_.commits;
        for (auto *record : surface.records) {
            record->pending = false;
        }
        if (!surface.configured) {
            Configure(surface);
        }
    }

    static void Frame(wl_client *client, wl_resource *resource, uint32_t id)
    {
        auto &surface = *static_cast<Surface *>(wl_resource_get_user_data(resource));
        auto *callback = wl_resource_create(client, &wl_callback_interface, 1, id);
        auto *record = new Record{&surface, callback, false};
        wl_resource_set_implementation(callback, nullptr, record, DestroyRecord);
        surface.records.push_back(record);
        ++surface.server->totals_.frames;
    }

    static void CreateSurface(wl_client *client, wl_resource *resource, uint32_t id)
    {
        auto *server = static_cast<Server *>(wl_resource_get_user_data(resource));
        auto *native = wl_resource_create(client, &wl_surface_interface, 4, id);
        auto *surface = new Surface{};
        surface->server = server;
        surface->resource = native;
        static const struct wl_surface_interface implementation = [] {
            struct wl_surface_interface result{};
            result.destroy = Destroy;
            result.attach = [](wl_client *, wl_resource *, wl_resource *, int32_t, int32_t) {
            };
            result.damage = [](wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {
            };
            result.frame = Frame;
            result.set_opaque_region = [](wl_client *, wl_resource *, wl_resource *) {
            };
            result.set_input_region = [](wl_client *, wl_resource *, wl_resource *) {
            };
            result.commit = Commit;
            result.set_buffer_transform = [](wl_client *, wl_resource *, int32_t) {
            };
            result.set_buffer_scale = [](wl_client *, wl_resource *, int32_t) {
            };
            result.damage_buffer = [](wl_client *, wl_resource *, int32_t, int32_t, int32_t,
                                      int32_t) {
            };
            return result;
        }();
        wl_resource_set_implementation(native, &implementation, surface, DestroySurface);
        server->current_ = surface;
        ++server->totals_.surfaces;
    }

    static void CreateRegion(wl_client *client, wl_resource *, uint32_t id)
    {
        static const struct wl_region_interface implementation{
            .destroy = Destroy,
            .add = [](wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {},
            .subtract =
                [](wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {
                }};
        auto *region = wl_resource_create(client, &wl_region_interface, 1, id);
        wl_resource_set_implementation(region, &implementation, nullptr, nullptr);
    }

    static void BindCompositor(wl_client *client, void *data, uint32_t version, uint32_t id)
    {
        static const struct wl_compositor_interface implementation{.create_surface = CreateSurface,
                                                                   .create_region = CreateRegion};
        auto *resource =
            wl_resource_create(client, &wl_compositor_interface, std::min(version, 4u), id);
        wl_resource_set_implementation(resource, &implementation, data, nullptr);
    }

    static void GetToplevel(wl_client *client, wl_resource *resource, uint32_t id)
    {
        auto *surface = static_cast<Surface *>(wl_resource_get_user_data(resource));
        static const struct xdg_toplevel_interface implementation = [] {
            struct xdg_toplevel_interface result{};
            result.destroy = Destroy;
            result.set_title = [](wl_client *, wl_resource *, const char *) {
            };
            result.set_app_id = [](wl_client *, wl_resource *, const char *) {
            };
            result.set_maximized = [](wl_client *, wl_resource *) {
            };
            result.unset_maximized = [](wl_client *, wl_resource *) {
            };
            return result;
        }();
        surface->toplevel = wl_resource_create(client, &xdg_toplevel_interface, 1, id);
        wl_resource_set_implementation(surface->toplevel, &implementation, surface,
                                       DestroyToplevel);
    }

    static void GetXdg(wl_client *client, wl_resource *, uint32_t id, wl_resource *native)
    {
        auto *surface = static_cast<Surface *>(wl_resource_get_user_data(native));
        static const struct xdg_surface_interface implementation = [] {
            struct xdg_surface_interface result{};
            result.destroy = Destroy;
            result.get_toplevel = GetToplevel;
            result.set_window_geometry = [](wl_client *, wl_resource *, int32_t, int32_t, int32_t,
                                            int32_t) {
            };
            result.ack_configure = [](wl_client *, wl_resource *, uint32_t) {
            };
            return result;
        }();
        surface->xdg = wl_resource_create(client, &xdg_surface_interface, 1, id);
        wl_resource_set_implementation(surface->xdg, &implementation, surface, DestroyXdg);
    }

    static void BindShell(wl_client *client, void *data, uint32_t, uint32_t id)
    {
        static const struct xdg_wm_base_interface implementation = [] {
            struct xdg_wm_base_interface result{};
            result.destroy = Destroy;
            result.get_xdg_surface = GetXdg;
            result.pong = [](wl_client *, wl_resource *, uint32_t) {
            };
            return result;
        }();
        auto *resource = wl_resource_create(client, &xdg_wm_base_interface, 1, id);
        wl_resource_set_implementation(resource, &implementation, data, nullptr);
    }

    static void Feedback(wl_client *client, wl_resource *, wl_resource *native, uint32_t id)
    {
        auto *surface = static_cast<Surface *>(wl_resource_get_user_data(native));
        auto *resource = wl_resource_create(client, &wp_presentation_feedback_interface, 1, id);
        auto *record = new Record{surface, resource, true};
        wl_resource_set_implementation(resource, nullptr, record, DestroyRecord);
        surface->records.push_back(record);
        ++surface->server->totals_.feedbacks;
    }

    static void BindPresentation(wl_client *client, void *data, uint32_t, uint32_t id)
    {
        // The generated client header supplies the shared interface symbols;
        // the server request vtable follows the two requests in protocol order.
        struct Implementation {
            void (*destroy)(wl_client *, wl_resource *);
            void (*feedback)(wl_client *, wl_resource *, wl_resource *, uint32_t);
        };

        static const Implementation implementation{Destroy, Feedback};
        auto *resource = wl_resource_create(client, &wp_presentation_interface, 1, id);
        wl_resource_set_implementation(resource, &implementation, data, nullptr);
        wl_resource_post_event(resource, 0, static_cast<uint32_t>(CLOCK_MONOTONIC));
    }

public:
    explicit Server(const char *socket)
    {
        display_ = wl_display_create();
        assert(display_);
        assert(wl_display_init_shm(display_) == 0);
        compositor_ = wl_global_create(display_, &wl_compositor_interface, 4, this, BindCompositor);
        shell_ = wl_global_create(display_, &xdg_wm_base_interface, 1, this, BindShell);
        presentation_ =
            wl_global_create(display_, &wp_presentation_interface, 1, this, BindPresentation);
        assert(compositor_ && shell_ && presentation_ &&
               wl_display_add_socket(display_, socket) == 0);
        thread_ = std::thread([&] {
            auto *loop = wl_display_get_event_loop(display_);
            while (!stop_) {
                assert(wl_event_loop_dispatch(loop, 5) >= 0);
                std::vector<std::function<void()>> tasks;
                {
                    std::lock_guard lock(mutex_);
                    tasks.swap(tasks_);
                }
                for (auto &task : tasks) {
                    task();
                }
                wl_display_flush_clients(display_);
            }
            wl_display_destroy_clients(display_);
        });
    }

    ~Server()
    {
        stop_ = true;
        thread_.join();
        wl_display_destroy(display_);
    }

    void Sync(std::function<void()> task)
    {
        std::promise<void> finished;
        auto future = finished.get_future();
        {
            std::lock_guard lock(mutex_);
            tasks_.push_back([&] {
                task();
                finished.set_value();
            });
        }
        assert(future.wait_for(2s) == std::future_status::ready);
        future.get();
    }

    Snapshot Inspect()
    {
        Snapshot result;
        Sync([&] {
            result = totals_;
            if (current_) {
                for (auto *record : current_->records) {
                    if (record->feedback) {
                        ++result.live_feedbacks;
                        if (record->pending) {
                            ++result.pending_feedbacks;
                        }
                    } else {
                        ++result.live_frames;
                        if (record->pending) {
                            ++result.pending_frames;
                        }
                    }
                }
            }
        });
        return result;
    }

    void Release()
    {
        Sync([&] {
            assert(current_);
            auto records = current_->records;
            for (auto *record : records) {
                if (!record->pending) {
                    if (record->feedback) {
                        wl_resource_post_event(record->resource, 1, 0u, 1u, 0u, 16666667u, 0u, 1u,
                                               0u);
                    } else {
                        wl_callback_send_done(record->resource, 1);
                    }
                    wl_resource_destroy(record->resource);
                }
            }
        });
    }

    void Resize(int width, int height)
    {
        Sync([&] {
            assert(current_);
            Configure(*current_, width, height);
        });
    }

    void ReleaseFrames()
    {
        Sync(std::bind_front(&Server::ReleaseFramesOnThread, this));
    }

    void ReleaseFeedback(unsigned index, bool discarded = false)
    {
        Sync(std::bind_front(&Server::ReleaseFeedbackOnThread, this, index, discarded));
    }
};

void Until(prism::platform::WaylandWindow &window, std::function<bool()> condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        assert(window.Pump(5));
    }
}

void UntilServer(Server &server, std::function<bool(const Snapshot &)> condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!condition(server.Inspect())) {
        assert(std::chrono::steady_clock::now() < deadline);
    }
}

struct IdentityFixture {
    prism::platform::WaylandWindow window;
    SubmitRequest request;
    bool pixels{true};
    std::vector<prism::platform::PixelPresentation> feedback;

    SubmitResult Prepare(const SubmitRequest &next)
    {
        request = next;
        return pixels && next.allow_pixels ? SubmitResult::Pixels : SubmitResult::None;
    }

    bool Commit()
    {
        wl_surface_commit(request.surface);
        pixels = false;
        return wl_display_flush(request.display) >= 0;
    }

    void Presented(const prism::platform::PixelPresentation &event)
    {
        feedback.push_back(event);
    }

    void Open()
    {
        window.SetSubmitHandlers(std::bind_front(&IdentityFixture::Prepare, this),
                                 std::bind_front(&IdentityFixture::Commit, this));
        window.SetPresentationHandler(std::bind_front(&IdentityFixture::Presented, this));
        assert(window.Open("wayland-submit-test", "identity.fixture", "Identity fixture", 160, 90));
        assert(!window.LastPixelSubmission());
    }

    void RequestPixels()
    {
        pixels = true;
        window.RequestUpdate(true);
    }

    bool PixelsAt(std::uint64_t count) const
    {
        return window.GetSubmitStats().pixel_commits == count;
    }

    bool FramesAt(int count) const
    {
        return window.FrameDoneCount() == count;
    }

    bool FeedbackAt(std::size_t count) const
    {
        return feedback.size() == count;
    }
};

struct DeferredFixture {
    prism::platform::WaylandWindow window;
    SubmitRequest request;
    unsigned preparations{}, commits{}, observations{};
    bool ready{};

    SubmitResult Prepare(const SubmitRequest &next)
    {
        ++preparations;
        request = next;
        assert(next.allow_pixels && next.force_pixels);
        return ready ? SubmitResult::Pixels : SubmitResult::Deferred;
    }

    bool Commit()
    {
        ++commits;
        wl_surface_commit(request.surface);
        return wl_display_flush(request.display) >= 0;
    }

    void Submitted(SubmitResult result)
    {
        assert(result == SubmitResult::Pixels);
        ++observations;
    }

    bool Prepared() const
    {
        return window.IsConfigured() && preparations > 0;
    }
};

void VerifyDeferredPreparation(Server &server)
{
    const auto before = server.Inspect();
    DeferredFixture fixture;
    fixture.window.SetSubmitHandlers(std::bind_front(&DeferredFixture::Prepare, &fixture),
                                     std::bind_front(&DeferredFixture::Commit, &fixture),
                                     std::bind_front(&DeferredFixture::Submitted, &fixture));
    assert(fixture.window.Open("wayland-submit-test", "deferred.fixture", "Deferred fixture", 160,
                               90));
    Until(fixture.window, std::bind_front(&DeferredFixture::Prepared, &fixture));
    UntilServer(server, [&before](const Snapshot &snapshot) {
        return snapshot.commits == before.commits + 1;
    });
    assert(fixture.window.Display() == fixture.request.display);
    assert(fixture.window.Surface() == fixture.request.surface);
    assert(fixture.preparations == 1 && fixture.commits == 0 && fixture.observations == 0);

    // Even an infinite caller wait must yield a Deferred owner turn, expose a
    // ready control descriptor, and prepare only once despite configure/frame
    // listeners also reaching TrySubmit. The caller still owns the FD data.
    const int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(wake >= 0);
    const std::uint64_t signal = 1;
    assert(write(wake, &signal, sizeof(signal)) == sizeof(signal));
    pollfd source{wake, POLLIN, 0};
    const auto calls = fixture.preparations;
    assert(fixture.window.Pump(-1, std::span(&source, 1)));
    assert(source.revents & POLLIN);
    assert(fixture.preparations == calls + 1);
    std::uint64_t received{};
    assert(read(wake, &received, sizeof(received)) == sizeof(received) && received == 1);
    for (unsigned turn = 0; turn < 3; ++turn) {
        const auto previous = fixture.preparations;
        assert(fixture.window.Pump(-1));
        assert(fixture.preparations == previous + 1);
    }
    const auto deferred = server.Inspect();
    const auto stats = fixture.window.GetSubmitStats();
    assert(deferred.commits == before.commits + 1 && deferred.frames == before.frames &&
           deferred.feedbacks == before.feedbacks);
    assert(stats.none == 0 && stats.state_commits == 0 && stats.pixel_commits == 0 &&
           stats.failures == 0);
    assert(!fixture.window.LastPixelSubmission() && !fixture.window.FrameCallbackPending());
    assert(fixture.commits == 0 && fixture.observations == 0);

    // Finishing bounded work consumes the original forced request. No second
    // RequestUpdate/RequestRedraw is needed and no submission ID was skipped.
    fixture.ready = true;
    assert(fixture.window.Pump(0));
    UntilServer(server, [&before](const Snapshot &snapshot) {
        return snapshot.commits == before.commits + 2 && snapshot.frames == before.frames + 1 &&
               snapshot.feedbacks == before.feedbacks + 1;
    });
    assert(fixture.window.LastPixelSubmission().value == 1);
    assert(fixture.window.GetSubmitStats().pixel_commits == 1);
    assert(fixture.window.FrameCallbackPending() && fixture.commits == 1 &&
           fixture.observations == 1);
    close(wake);
    fixture.window.Close();
    assert(!fixture.window.Display() && !fixture.window.Surface());
    UntilServer(server, [](const Snapshot &snapshot) { return snapshot.surfaces == 0; });
}

void VerifyOutOfOrderFeedback(Server &server)
{
    IdentityFixture fixture;
    fixture.Open();
    Until(fixture.window, std::bind_front(&IdentityFixture::PixelsAt, &fixture, 1));
    UntilServer(server, [](const Snapshot &snapshot) {
        return snapshot.live_frames == 1 && snapshot.live_feedbacks == 1 &&
               snapshot.pending_frames == 0;
    });
    assert(fixture.window.LastPixelSubmission().value == 1);
    server.ReleaseFrames();
    Until(fixture.window, std::bind_front(&IdentityFixture::FramesAt, &fixture, 1));
    fixture.RequestPixels();
    Until(fixture.window, std::bind_front(&IdentityFixture::PixelsAt, &fixture, 2));
    UntilServer(server, [](const Snapshot &snapshot) {
        return snapshot.live_frames == 1 && snapshot.live_feedbacks == 2 &&
               snapshot.pending_frames == 0;
    });

    // Present the newer pixels first, while both feedback objects exist. A
    // later old-frame discard must neither acquire the new ID nor retire its
    // still outstanding frame callback.
    server.ReleaseFeedback(1);
    Until(fixture.window, std::bind_front(&IdentityFixture::FeedbackAt, &fixture, 1));
    assert(fixture.feedback[0].submission.value == 2);
    assert(fixture.feedback[0].outcome == prism::platform::PresentationOutcome::Presented);
    server.ReleaseFeedback(0, true);
    Until(fixture.window, std::bind_front(&IdentityFixture::FeedbackAt, &fixture, 2));
    assert(fixture.feedback[1].submission.value == 1);
    assert(fixture.feedback[1].outcome == prism::platform::PresentationOutcome::Discarded);
    assert(fixture.window.FrameCallbackPending());
    assert(fixture.window.LastPixelSubmission().value == 2);
    assert(fixture.window.GetSubmitStats().pixel_commits == 2);
    assert(fixture.window.PresentationCount() == 1 && fixture.window.DiscardedCount() == 1);
    server.ReleaseFrames();
    Until(fixture.window, std::bind_front(&IdentityFixture::FramesAt, &fixture, 2));
    fixture.window.Close();
    UntilServer(server, [](const Snapshot &snapshot) { return snapshot.surfaces == 0; });
}

void VerifyFeedbackBackpressure(Server &server)
{
    IdentityFixture fixture;
    fixture.Open();
    for (std::uint64_t submission = 1; submission <= 8; ++submission) {
        Until(fixture.window, std::bind_front(&IdentityFixture::PixelsAt, &fixture, submission));
        UntilServer(server, [submission](const Snapshot &snapshot) {
            return snapshot.live_frames == 1 && snapshot.live_feedbacks == submission &&
                   snapshot.pending_frames == 0;
        });
        server.ReleaseFrames();
        Until(fixture.window,
              std::bind_front(&IdentityFixture::FramesAt, &fixture, static_cast<int>(submission)));
        if (submission < 8) {
            fixture.RequestPixels();
        }
    }

    fixture.RequestPixels();
    assert(fixture.window.Pump(0));
    assert(!fixture.request.allow_pixels && !fixture.window.FrameCallbackPending());
    assert(fixture.window.GetSubmitStats().pixel_commits == 8);
    assert(fixture.window.LastPixelSubmission().value == 8);
    assert(server.Inspect().live_feedbacks == 8);

    // Freeing one presentation slot alone wakes deferred pixels; no new
    // RequestPixels is required, and the submission ID is never skipped.
    server.ReleaseFeedback(0);
    Until(fixture.window, std::bind_front(&IdentityFixture::PixelsAt, &fixture, 9));
    UntilServer(server, [](const Snapshot &snapshot) {
        return snapshot.live_frames == 1 && snapshot.live_feedbacks == 8 &&
               snapshot.pending_frames == 0;
    });
    assert(fixture.window.LastPixelSubmission().value == 9);
    assert(fixture.feedback.size() == 1 && fixture.feedback[0].submission.value == 1);
    server.Release();
    Until(fixture.window, std::bind_front(&IdentityFixture::FeedbackAt, &fixture, 9));
    Until(fixture.window, std::bind_front(&IdentityFixture::FramesAt, &fixture, 9));
    fixture.window.Close();
    UntilServer(server, [](const Snapshot &snapshot) { return snapshot.surfaces == 0; });
}
} // namespace

int main()
{
    char path[] = "/tmp/prism-submit-test.XXXXXX";
    assert(mkdtemp(path));
    setenv("XDG_RUNTIME_DIR", path, 1);
    {
        Server server("wayland-submit-test");
        prism::platform::WaylandWindow window;
        SubmitResult demand = SubmitResult::Pixels;
        SubmitRequest last{};
        bool dirty_pixels = true;
        std::vector<SubmitResult> observed;
        window.SetSubmitHandlers(
            [&](const SubmitRequest &request) {
                last = request;
                if (demand == SubmitResult::State) {
                    const prism::contracts::SurfaceInputRegion input{{0, 0, 80, 40}, 0};
                    window.SetInputRegions(std::span(&input, 1));
                    return SubmitResult::State;
                }
                if (demand == SubmitResult::Pixels && dirty_pixels) {
                    return request.allow_pixels ? SubmitResult::Pixels : SubmitResult::None;
                }
                return SubmitResult::None;
            },
            [&] {
                wl_surface_commit(last.surface);
                dirty_pixels = false;
                return wl_display_flush(last.display) >= 0;
            },
            [&](SubmitResult result) {
                observed.push_back(result);
                if (result == SubmitResult::State) {
                    demand = SubmitResult::None;
                }
            });
        assert(window.Open("wayland-submit-test", "submit.fixture", "Submit fixture", 160, 90));
        Until(window, [&] { return window.GetSubmitStats().pixel_commits == 1; });
        UntilServer(server, [](const Snapshot &s) {
            return s.frames == 1 && s.feedbacks == 1 && s.commits == 2;
        });
        assert(window.FrameCallbackPending());
        assert(window.LastPixelSubmission().value == 1);
        auto before = server.Inspect();
        demand = SubmitResult::None;
        window.RequestUpdate(true);
        assert(window.Pump(0));
        auto after = server.Inspect();
        assert(after.commits == before.commits && after.frames == before.frames &&
               after.feedbacks == before.feedbacks);
        assert(window.GetSubmitStats().none >= 1);
        assert(window.LastPixelSubmission().value == 1);
        demand = SubmitResult::State;
        window.RequestUpdate(true);
        Until(window, [&] { return window.GetSubmitStats().state_commits == 1; });
        assert(window.Pump(0)); // Flush the State commit independently of pixels.
        UntilServer(server, [&](const Snapshot &s) { return s.commits == before.commits + 1; });
        after = server.Inspect();
        assert(after.commits == before.commits + 1 && after.frames == before.frames &&
               after.feedbacks == before.feedbacks);
        assert(window.FrameCallbackPending() && after.pending_frames == 0 &&
               after.live_frames == 1);
        assert(window.LastPixelSubmission().value == 1);
        demand = SubmitResult::Pixels;
        dirty_pixels = true;
        window.RequestUpdate(true);
        assert(window.Pump(0));
        assert(window.GetSubmitStats().pixel_commits == 1);
        assert(server.Inspect().frames == 1);
        // Pixel work deferred behind the old callback must remain demanded;
        // completing that callback retries preparation without another request.
        server.Release();
        Until(window, [&] {
            return window.FrameDoneCount() == 1 && window.GetSubmitStats().pixel_commits == 2;
        });
        UntilServer(server, [](const Snapshot &s) {
            return s.frames == 2 && s.feedbacks == 2 && s.commits == 4;
        });
        assert(server.Inspect().frames == 2 && window.FrameCallbackPending());
        server.Release();
        Until(window, [&] { return window.FrameDoneCount() == 2; });
        // Same-size configure needs a State commit for its acknowledgement,
        // without requesting another pixel callback or presentation object.
        demand = SubmitResult::None;
        before = server.Inspect();
        const int configured = window.ConfigureCount();
        server.Resize(160, 90);
        Until(window, [&] { return window.ConfigureCount() > configured; });
        assert(window.Pump(0)); // Flush the configure acknowledgement State.
        UntilServer(server, [&](const Snapshot &s) { return s.commits == before.commits + 1; });
        after = server.Inspect();
        assert(after.frames == before.frames && after.feedbacks == before.feedbacks &&
               after.commits == before.commits + 1);
        // A true resize carries force_pixels. The next actual pixel submission
        // works after metadata-only configure and drained callbacks.
        before = server.Inspect();
        demand = SubmitResult::Pixels;
        dirty_pixels = true;
        server.Resize(200, 110);
        Until(window, [&] { return window.GetSubmitStats().pixel_commits == 3; });
        UntilServer(server, [&](const Snapshot &s) {
            return s.frames == before.frames + 1 && s.feedbacks == before.feedbacks + 1 &&
                   s.commits == before.commits + 1;
        });
        assert(last.width == 200 && last.height == 110 && last.force_pixels);
        server.Release();
        Until(window, [&] { return window.FrameDoneCount() == 3; });
        window.Close();
        UntilServer(server, [](const Snapshot &s) { return s.surfaces == 0; });
        // Preparation failures request no frame callback or presentation object.
        before = server.Inspect();
        prism::platform::WaylandWindow failed_prepare;
        failed_prepare.SetSubmitHandlers([](const SubmitRequest &) { return SubmitResult::Failed; },
                                         [] {
                                             assert(false);
                                             return false;
                                         });
        assert(failed_prepare.Open("wayland-submit-test", "prepare.failure", "Prepare failure", 160,
                                   90));
        for (int i = 0; i < 100 && !failed_prepare.GetSubmitStats().failures; ++i) {
            failed_prepare.Pump(5);
        }
        assert(failed_prepare.GetSubmitStats().failures == 1);
        assert(!failed_prepare.LastPixelSubmission());
        UntilServer(server, [](const Snapshot &s) { return s.surfaces == 0; });
        after = server.Inspect();
        assert(after.frames == before.frames && after.feedbacks == before.feedbacks);
        // A failed Swap may already have pending server objects. Force requests
        // onto the connection before failing, proving terminal Close destroys
        // them instead of allowing a subsequent State commit to capture them.
        before = server.Inspect();
        prism::platform::WaylandWindow failed_swap;
        SubmitRequest failure_request{};
        Snapshot pending{};
        failed_swap.SetSubmitHandlers(
            [&](const SubmitRequest &request) {
                failure_request = request;
                return SubmitResult::Pixels;
            },
            [&] {
                assert(wl_display_flush(failure_request.display) >= 0);
                UntilServer(server, [&](const Snapshot &s) {
                    return s.frames == before.frames + 1 && s.feedbacks == before.feedbacks + 1;
                });
                pending = server.Inspect();
                return false;
            });
        assert(failed_swap.Open("wayland-submit-test", "swap.failure", "Swap failure", 160, 90));
        for (int i = 0; i < 100 && !failed_swap.GetSubmitStats().failures; ++i) {
            failed_swap.Pump(5);
        }
        assert(failed_swap.GetSubmitStats().failures == 1 && pending.pending_frames == 1 &&
               pending.pending_feedbacks == 1);
        assert(!failed_swap.LastPixelSubmission());
        assert(!failed_swap.PresentationPending({1}));
        UntilServer(server, [](const Snapshot &s) { return s.surfaces == 0; });
        after = server.Inspect();
        assert(after.commits == before.commits + 1 && after.live_frames == 0 &&
               after.live_feedbacks == 0);
        for (int i = 0; i < 20; ++i) {
            failed_swap.RequestUpdate(true);
            assert(!failed_swap.Pump(0));
        }
        const auto stable = server.Inspect();
        assert(stable.commits == after.commits && stable.frames == after.frames &&
               stable.feedbacks == after.feedbacks);
        assert(failed_swap.GetSubmitStats().failures == 1);
        VerifyOutOfOrderFeedback(server);
        VerifyFeedbackBackpressure(server);
        VerifyDeferredPreparation(server);
    }
    std::filesystem::remove_all(path);
}
