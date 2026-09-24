#include "prism/launcher/zygote_server.hpp"
#include "prism/core/logging.hpp"
#include "prism/core/types.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstring>
#include <cerrno>

namespace prism::launcher {

ZygoteServer::ZygoteServer() = default;

ZygoteServer::~ZygoteServer() {
    Stop();
}

void ZygoteServer::Prewarm() {
    PRISM_LOG_INFO("ZYGOTE", "Pre-warming shared runtime memory pages (COW zero-linking)...");
    dlopen("libc.so.6", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libm.so.6", RTLD_NOW | RTLD_GLOBAL);
    PRISM_LOG_INFO("ZYGOTE", "System runtime and Wayland client libraries locked into warm cache");
}

bool ZygoteServer::Start(const std::string& socket_path) {
    socket_path_ = socket_path;
    unlink(socket_path_.c_str());

    Prewarm();

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        PRISM_LOG_ERROR("ZYGOTE", "Failed to create UNIX domain socket: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        PRISM_LOG_ERROR("ZYGOTE", "Failed to bind socket: %s", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 16) < 0) {
        PRISM_LOG_ERROR("ZYGOTE", "Failed to listen: %s", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    PRISM_LOG_INFO("ZYGOTE", "Prism Zygote Server ready and listening on %s", socket_path_.c_str());
    RunLoop();
    return true;
}

void ZygoteServer::Stop() {
    if (running_) {
        running_ = false;
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
        unlink(socket_path_.c_str());
        PRISM_LOG_INFO("ZYGOTE", "Zygote Server stopped");
    }
}

void ZygoteServer::RunLoop() {
    while (running_) {
        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        HandleClient(client_fd);

        int status;
        while (waitpid(-1, &status, WNOHANG) > 0) {}
    }
}

void ZygoteServer::HandleClient(int client_fd) {
    char buffer[256];
    ssize_t bytes = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes <= 0) {
        close(client_fd);
        return;
    }

    buffer[bytes] = '\0';
    std::string cmd(buffer);
    PRISM_LOG_INFO("ZYGOTE", "Fork request received: '%s'", cmd.c_str());

    auto t0 = core::CurrentTimeNs();
    pid_t pid = fork();

    if (pid == 0) {
        // Child Backend Process
        close(client_fd);
        close(server_fd_);
        PRISM_LOG_INFO("APP-CHILD", "Child backend spawned! PID: %d, executing...", getpid());
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    } else if (pid > 0) {
        // Parent Zygote Daemon
        auto elapsed_us = (core::CurrentTimeNs() - t0) / 1000.0;
        PRISM_LOG_INFO("ZYGOTE", "fork() executed in %.2f us (%.3f ms), child PID: %d", elapsed_us, elapsed_us / 1000.0, pid);
        std::string reply = "OK " + std::to_string(pid) + "\n";
        ssize_t w = write(client_fd, reply.data(), reply.size());
        (void)w;
        close(client_fd);
    }
}

} // namespace prism::launcher
