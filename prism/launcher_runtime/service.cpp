#include "prism/theme/compiler.hpp"
#include "service_p.hpp"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace prism::launcher {
using detail::Now;
using detail::Require;

namespace detail {
std::uint64_t Now()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Require(bool valid, const char *detail)
{
    if (!valid) {
        throw std::runtime_error(detail);
    }
}
} // namespace detail

Service::Impl::Impl(ServiceConfig value) : config(std::move(value))
{
}

Service::Impl::~Impl()
{
    Shutdown();
    if (listener >= 0) {
        close(listener);
    }
    struct stat info{};
    if (socket_inode && !lstat(socket_path.c_str(), &info) && info.st_dev == socket_device &&
        info.st_ino == socket_inode) {
        unlink(socket_path.c_str());
    }
    if (lock >= 0) {
        close(lock);
    }
}

void Service::Impl::Open()
{
    opened_at = Now();
    if (config.themes_root.empty()) {
        config.themes_root = prism::theme::DefaultThemeRoot();
    }

    theme = prism::theme::LoadTheme(config.themes_root, config.theme_id, 1, config.color_scheme);
    committed_theme = theme;
    theme_ready = config.wm_fd < 0;
    if (config.wm_fd >= 0) {
        launch::VerifyControlPeer(config.wm_fd, config.parent_pid);
        control = std::make_unique<launch::Stream>(config.wm_fd, launch::ControlFrameSize);
    }

    Require(!config.start_shell || control, "Shell bootstrap requires trusted WM control");
    Require(config.pool_size <= config.max_workers && config.max_workers >= 1 &&
                config.max_workers <= 32,
            "Pool/max worker counts are invalid");
    load_budget = runtime::SessionTaskBudget::Create(config.load_budget);
    struct sigaction child_action{};
    Require(!sigaction(SIGCHLD, nullptr, &child_action) && child_action.sa_handler != SIG_IGN &&
                !(child_action.sa_flags & SA_NOCLDWAIT),
            "Launcher requires ownership of SIGCHLD/waitpid");

    config.apps_root = std::filesystem::canonical(config.apps_root);
    config.host = std::filesystem::canonical(config.host);
    Require(std::filesystem::is_directory(config.apps_root) &&
                access(config.host.c_str(), X_OK) == 0,
            "Registry or host path is invalid");
    Require(!config.socket_name.empty() && config.socket_name.size() < 64 &&
                config.socket_name.find('/') == std::string::npos && config.socket_name != "." &&
                config.socket_name != "..",
            "Socket must be a basename");

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    Require(runtime && *runtime, "XDG_RUNTIME_DIR is required");
    struct stat info{};
    Require(!lstat(runtime, &info) && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
                (info.st_mode & 0777) == 0700,
            "Runtime directory must belong to this user and have mode 0700");
    auto directory = std::filesystem::path(runtime) / "prism";
    if (mkdir(directory.c_str(), 0700) && errno != EEXIST) {
        throw std::runtime_error("Cannot create launcher directory");
    }
    Require(!lstat(directory.c_str(), &info) && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
                (info.st_mode & 0777) == 0700,
            "Launcher directory is not private");

    socket_path = directory / config.socket_name;
    const auto lock_path = socket_path.string() + ".lock";
    lock = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    Require(lock >= 0 && !fstat(lock, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                (info.st_mode & 0777) == 0600 && !flock(lock, LOCK_EX | LOCK_NB),
            "Launcher endpoint is already locked or unsafe");

    sockaddr_un address{};
    Require(socket_path.string().size() < sizeof(address.sun_path),
            "Launcher socket path is too long");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.string().size() + 1);
    if (!lstat(socket_path.c_str(), &info)) {
        Require(S_ISSOCK(info.st_mode) && info.st_uid == geteuid(),
                "Refusing to replace a non-socket endpoint");
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        Require(probe >= 0, "Socket probe failed");
        const int result = connect(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        const int error = errno;
        close(probe);
        Require(result < 0 && error == ECONNREFUSED, "An existing endpoint is still live");
        Require(!unlink(socket_path.c_str()), "Cannot remove stale endpoint");
    } else {
        Require(errno == ENOENT, "Cannot inspect launcher socket");
    }

    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    Require(listener >= 0, "Cannot create launcher socket");
    const auto mask = umask(0077);
    const int result = bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    umask(mask);
    Require(!result, "Cannot bind launcher socket");
    Require(!lstat(socket_path.c_str(), &info), "Cannot inspect bound socket");
    socket_device = info.st_dev;
    socket_inode = info.st_ino;
    Require(!chmod(socket_path.c_str(), 0600) && !listen(listener, 32),
            "Cannot listen on launcher socket");

    std::cout << "launcher socket=" << socket_path << " pool=" << config.pool_size << std::endl;
}

Endpoint *Service::Impl::Find(Owner owner)
{
    if (owner.worker) {
        auto it = workers.find(static_cast<pid_t>(owner.endpoint));
        return it == workers.end() ? nullptr : &it->second;
    }
    auto it = clients.find(owner.endpoint);
    return it == clients.end() ? nullptr : &it->second;
}

void Service::Impl::Accept()
{
    for (unsigned count = 0; count < 16; ++count) {
        const int fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            break;
        }
        ucred credential{};
        socklen_t length = sizeof(credential);
        if (clients.size() >= 64 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &length) ||
            credential.uid != geteuid()) {
            close(fd);
            continue;
        }
        Endpoint endpoint;
        endpoint.stream = std::make_unique<launch::Stream>(fd, launch::FrameSize);
        clients.emplace(next_endpoint++, std::move(endpoint));
    }
}

Service::Service(ServiceConfig config) : impl_(std::make_unique<Impl>(std::move(config)))
{
}

Service::~Service() = default;

int Service::Run(const volatile std::sig_atomic_t &stopping, int signal_fd)
{
    return impl_->Run(stopping, signal_fd);
}
} // namespace prism::launcher
