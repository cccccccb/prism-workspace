#include "prism/platform/wayland_window.hpp"
#include "prism/wm/compositor.hpp"
#include "prism/wm/wlr_server.hpp"
#include "prism/ipc/ipc_server.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
extern "C" {
#include <wlr/types/wlr_xdg_shell.h>
}

using namespace prism;
using namespace std::chrono_literals;

struct Client {
    std::atomic<bool> stop{}, closed{};
    std::atomic<int> width{}, height{};
    std::thread thread;
    Client(const std::string& socket, int id) : thread([this, socket, id] {
        platform::WaylandWindow window;
        window.SetPaintHandler([](void* data, int w, int h, int stride) {
            auto* pixels = static_cast<std::uint32_t*>(data);
            for (int y=0; y<h; ++y) for (int x=0; x<w; ++x) pixels[y*stride/4+x] = 0xFF7799AA;
        });
        if (!window.Open(socket, "prism.bsp-"+std::to_string(id), "BSP \"window\" "+std::to_string(id), 500, 300)) return;
        while (!stop && window.Pump(20)) {
            width = window.Metrics().buffer_size.width;
            height = window.Metrics().buffer_size.height;
        }
        closed = window.IsCloseRequested();
    }) {}
    ~Client() { stop = true; thread.join(); }
};

static void Until(wm::WlrServer& server, const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now()+5s;
    while (!condition() && std::chrono::steady_clock::now()<deadline) server.RunEventLoopIteration(5);
    assert(condition());
}

static nlohmann::json Command(wm::WlrServer& server, const std::string& command) {
    const int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK, 0);
    assert(fd>=0);
    sockaddr_un address{}; address.sun_family=AF_UNIX;
    const auto& path = server.GetIpcServer()->GetSocketPath();
    assert(path.size()<sizeof(address.sun_path));
    std::copy(path.begin(), path.end(), address.sun_path);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))==0);
    const auto request=command+"\n";
    assert(send(fd, request.data(), request.size(), 0)==static_cast<ssize_t>(request.size()));
    std::string response;
    Until(server, [&] {
        char bytes[4096]; const auto size=recv(fd, bytes, sizeof(bytes), 0);
        if (size>0) response.append(bytes, size);
        return response.find('\n')!=std::string::npos;
    });
    close(fd);
    return nlohmann::json::parse(response);
}

static std::vector<nlohmann::json> Views(const nlohmann::json& node) {
    std::vector<nlohmann::json> result;
    if (node.value("type", "")=="view") result.push_back(node);
    for (const auto* key : {"nodes", "workspaces"}) if (node.contains(key)) for (const auto& child : node[key]) {
        auto children=Views(child); result.insert(result.end(), children.begin(), children.end());
    }
    return result;
}

static wl_iterator_result CountImplicitGeometry(wl_resource* resource, void* data) {
    if (std::strcmp(wl_resource_get_class(resource), "xdg_surface") == 0) {
        auto* surface = wlr_xdg_surface_from_resource(resource);
        if (surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && surface->surface->mapped &&
            surface->current.geometry.width == 0 && surface->current.geometry.height == 0)
            ++*static_cast<int*>(data);
    }
    return WL_ITERATOR_CONTINUE;
}

int main() {
    char directory[]="/tmp/prism-native-bsp.XXXXXX";
    assert(mkdtemp(directory));
    setenv("XDG_RUNTIME_DIR", directory, 1);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    auto compositor=std::make_shared<wm::Compositor>();
    assert(compositor->Initialize());
    wm::WlrServer server(compositor);
    const auto display="wayland-bsp-"+std::to_string(getpid());
    assert(server.Initialize(display)); server.Start();
    std::array<std::unique_ptr<Client>,4> clients;
    auto launch=[&](int id) {
        clients[id]=std::make_unique<Client>(display,id);
        Until(server,[&] { return compositor->GetWindows().size()==static_cast<size_t>(id+1); });
        assert(compositor->GetWindows().back()->GetCommittedBounds().width > 0);
        assert(compositor->GetWindows().back()->GetCommittedBounds().height > 0);
    };
    auto settled=[&] {
        for (std::size_t i=0; i<clients.size(); ++i) if (clients[i]) {
            const auto& window=compositor->GetWindows()[i];
            if (!window->IsVisible()) continue;
            const auto bounds=window->GetBounds();
            if (clients[i]->width!=static_cast<int>(bounds.width) || clients[i]->height!=static_cast<int>(bounds.height)) return false;
            if (window->GetCommittedBounds() != bounds) return false;
        }
        return true;
    };
    launch(0); launch(1); Until(server,settled);
    assert(Command(server,"focus left")["status"]=="ok");
    assert(Command(server,"split v")["status"]=="ok");
    launch(2); Until(server,settled);
    assert(Command(server,"focus right")["status"]=="ok");
    assert(Command(server,"split v")["status"]=="ok");
    launch(3); Until(server,settled);
    const auto windows=compositor->GetWindows();
    assert(windows.size()==4);
    // These actual SHM clients deliberately omit set_window_geometry. Verify
    // the fallback path is exercised, rather than inferring it from dimensions.
    int implicit_geometry=0;
    wl_client* client;
    wl_client_for_each(client, wl_display_get_client_list(server.GetDisplay()))
        wl_client_for_each_resource(client, CountImplicitGeometry, &implicit_geometry);
    assert(implicit_geometry==4);
    for (const auto& window : windows) assert(window->IsNative() && !window->GetChannel());
    const auto a=windows[0]->GetBounds(), b=windows[1]->GetBounds(), c=windows[2]->GetBounds(), d=windows[3]->GetBounds();
    assert(a.x==c.x && b.x==d.x && a.x<b.x && a.y<c.y && b.y<d.y);
    assert(a.x+a.width<b.x && a.y+a.height<c.y);
    assert(a.y>=wm::ThemeGeometry{}.topbar_surface_height);
    assert(d.y+d.height<compositor->GetScreenHeight()-wm::ThemeGeometry{}.dock_surface_height);
    const auto tree=Command(server,"get_tree");
    const auto views=Views(tree);
    assert(views.size()==4);
    int focused=0;
    for (const auto& view : views) {
        assert(view["native"]==true && view["pid"]==getpid());
        assert(view["name"].get<std::string>().find('"')!=std::string::npos);
        assert(view["committed_rect"]==view["rect"]);
        if (view["focused"]==true) ++focused;
    }
    assert(focused==1);
    assert(Command(server,"swap up")["status"]=="ok"); Until(server,settled);
    // The swap preserves dimensions; no subsequent client commit is required
    // to report the new displayed position in get_tree.
    for (const auto& view : Views(Command(server,"get_tree"))) assert(view["committed_rect"]==view["rect"]);
    assert(Command(server,"fullscreen on")["fullscreen"]==true); Until(server,settled);
    assert(compositor->GetTreeEngine().GetFocusedWindow()->GetBounds().width==compositor->GetScreenWidth());
    assert(Command(server,"fullscreen off")["fullscreen"]==false); Until(server,settled);
    assert(Command(server,"move_workspace 2")["status"]=="ok");
    int visible=0; for (const auto& window : windows) if (window->IsVisible()) ++visible;
    assert(visible==3);
    assert(Command(server,"workspace 2")["status"]=="ok");
    visible=0; for (const auto& window : windows) if (window->IsVisible()) ++visible;
    assert(visible==1);
    auto moved=compositor->GetTreeEngine().GetFocusedWindow();
    assert(Command(server,"close")["status"]=="ok");
    Until(server,[&] { return compositor->GetWindows().size()==3; });
    assert(compositor->GetTreeEngine().GetActiveWorkspace()->GetViewCount()==0);
    assert(Command(server,"workspace 1")["status"]=="ok");
    assert(compositor->GetTreeEngine().GetFocusedWindow() && compositor->GetTreeEngine().GetFocusedWindow()!=moved);
    clients={};
    Until(server,[&] { return compositor->GetWindows().empty(); });
    assert(Views(Command(server,"get_tree")).empty());
    server.Stop(); std::filesystem::remove_all(directory);
    std::puts("Native XDG BSP: 4 nested clients, target configure, real IPC focus/swap/fullscreen/workspaces/close passed");
}
