#pragma once

#include "prism/core/noncopyable.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct wl_event_loop;
struct wl_event_source;

namespace prism::ipc {

using IpcHandler =
    std::function<std::string(const std::string &cmd, const std::vector<std::string> &args)>;

/**
 * @brief Compositor-level IPC Server (Sway/i3 architecture).
 *        Allows CLI tools (prism-msg) and external controllers to query outputs,
 *        dynamically adjust resolution/refresh rate, change layouts, and trigger UI events.
 */
class IpcServer : public core::NonCopyable {
public:
    explicit IpcServer(struct wl_event_loop *loop);
    ~IpcServer();

    bool Start(const std::string &socket_path = "");
    void Stop();

    void RegisterHandler(const std::string &command, IpcHandler handler);
    void SetDefaultHandler(IpcHandler handler);

    const std::string &GetSocketPath() const
    {
        return socket_path_;
    }

    bool IsRunning() const
    {
        return server_fd_ >= 0;
    }

    // Internal event callbacks for wl_event_loop
    void HandleConnection();
    void HandleClientData(int client_fd);

private:
    struct wl_event_loop *loop_{nullptr};
    struct wl_event_source *listen_source_{nullptr};
    int server_fd_{-1};
    std::string socket_path_;

    std::unordered_map<std::string, IpcHandler> handlers_;
    IpcHandler default_handler_;

    struct Client {
        int fd{-1};
        struct wl_event_source *source{nullptr};
        std::string buffer;
    };

    std::vector<std::unique_ptr<Client>> clients_;

    void RemoveClient(int client_fd);
};

} // namespace prism::ipc
