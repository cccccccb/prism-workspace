#pragma once
#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
namespace prism::launcher {
struct ServiceConfig {
    std::filesystem::path apps_root, host;
    std::string socket_name{"launcher.sock"}, wayland;
    int wm_fd{-1}, parent_pid{};
    bool start_shell{};
    unsigned pool_size{1}, max_workers{8}, startup_timeout_ms{15000};
};
class Service {
public:
    explicit Service(ServiceConfig config);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    int Run(const volatile std::sig_atomic_t& stopping);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace prism::launcher
