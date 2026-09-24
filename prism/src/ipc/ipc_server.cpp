#include "prism/ipc/ipc_server.hpp"
#include "prism/core/logging.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <wayland-server-core.h>

#include <sstream>
#include <algorithm>

namespace prism::ipc {

static int handle_server_listen(int fd, uint32_t mask, void* data) {
    auto* server = static_cast<IpcServer*>(data);
    if (mask & WL_EVENT_READABLE) {
        server->HandleConnection();
    }
    return 0;
}

static int handle_client_readable(int fd, uint32_t mask, void* data) {
    auto* server = static_cast<IpcServer*>(data);
    if (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        server->HandleClientData(fd);
    }
    return 0;
}

IpcServer::IpcServer(struct wl_event_loop* loop)
    : loop_(loop) {}

IpcServer::~IpcServer() {
    Stop();
}

bool IpcServer::Start(const std::string& socket_path) {
    if (server_fd_ >= 0) return true;

    if (!socket_path.empty()) {
        socket_path_ = socket_path;
    } else {
        const char* runtime_dir = getenv("XDG_RUNTIME_DIR");
        if (runtime_dir && access(runtime_dir, W_OK) == 0) {
            socket_path_ = std::string(runtime_dir) + "/prism-ipc.sock";
        } else {
            socket_path_ = "/tmp/prism-ipc-" + std::to_string(getuid()) + ".sock";
        }
    }

    unlink(socket_path_.c_str());

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        PRISM_LOG_ERROR("IPC-SERVER", "Failed to create UNIX domain socket: %s", strerror(errno));
        return false;
    }

    // Set non-blocking
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        PRISM_LOG_ERROR("IPC-SERVER", "Failed to bind socket to '%s': %s", socket_path_.c_str(), strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 16) < 0) {
        PRISM_LOG_ERROR("IPC-SERVER", "Failed to listen on socket '%s': %s", socket_path_.c_str(), strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        unlink(socket_path_.c_str());
        return false;
    }

    if (loop_) {
        listen_source_ = wl_event_loop_add_fd(loop_, server_fd_, WL_EVENT_READABLE, handle_server_listen, this);
    }

    setenv("PRISMSOCK", socket_path_.c_str(), 1);
    PRISM_LOG_INFO("IPC-SERVER", "Prism IPC Server active on socket '%s' (exported PRISMSOCK)", socket_path_.c_str());
    return true;
}

void IpcServer::Stop() {
    if (server_fd_ < 0) return;

    for (auto& client : clients_) {
        if (client->source) {
            wl_event_source_remove(client->source);
        }
        if (client->fd >= 0) {
            close(client->fd);
        }
    }
    clients_.clear();

    if (listen_source_) {
        wl_event_source_remove(listen_source_);
        listen_source_ = nullptr;
    }

    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }

    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
    }

    PRISM_LOG_INFO("IPC-SERVER", "Prism IPC Server stopped cleanly");
}

void IpcServer::RegisterHandler(const std::string& command, IpcHandler handler) {
    handlers_[command] = std::move(handler);
}

void IpcServer::SetDefaultHandler(IpcHandler handler) {
    default_handler_ = std::move(handler);
}

void IpcServer::HandleConnection() {
    struct sockaddr_un client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
    if (client_fd < 0) return;

    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    auto client = std::make_unique<Client>();
    client->fd = client_fd;
    if (loop_) {
        client->source = wl_event_loop_add_fd(loop_, client_fd, WL_EVENT_READABLE, handle_client_readable, this);
    }
    clients_.push_back(std::move(client));
}

void IpcServer::HandleClientData(int client_fd) {
    auto it = std::find_if(clients_.begin(), clients_.end(),
                           [client_fd](const std::unique_ptr<Client>& c) { return c->fd == client_fd; });
    if (it == clients_.end()) return;
    Client* client = it->get();

    char buf[1024];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        // EOF or error -> close
        RemoveClient(client_fd);
        return;
    }

    buf[n] = '\0';
    client->buffer.append(buf, n);

    // Process complete command lines
    size_t newline_pos = client->buffer.find('\n');
    if (newline_pos != std::string::npos) {
        std::string line = client->buffer.substr(0, newline_pos);
        client->buffer.erase(0, newline_pos + 1);

        // Trim carriage return
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Parse command and args
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }

        std::string response;
        auto handler_it = handlers_.find(cmd);
        if (handler_it != handlers_.end()) {
            response = handler_it->second(cmd, args);
        } else if (default_handler_) {
            response = default_handler_(cmd, args);
        } else {
            response = "{\"status\": \"error\", \"message\": \"Unknown command: " + cmd + "\"}";
        }

        response += "\n";
        (void)write(client_fd, response.data(), response.size());

        // Finished request-response: disconnect client
        RemoveClient(client_fd);
    }
}

void IpcServer::RemoveClient(int client_fd) {
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        if ((*it)->fd == client_fd) {
            if ((*it)->source) {
                wl_event_source_remove((*it)->source);
            }
            close((*it)->fd);
            clients_.erase(it);
            break;
        }
    }
}

} // namespace prism::ipc
