#include "prism/launcher/service.hpp"
#include <charconv>
#include <csignal>
#include <iostream>
#include <unistd.h>
#include <sys/prctl.h>
namespace {
volatile std::sig_atomic_t stopping = 0;
void Stop(int) { stopping = 1; }
}
int main(int argc, char** argv) {
    prism::launcher::ServiceConfig config;
    const auto bin = std::filesystem::canonical("/proc/self/exe").parent_path();
    config.host = bin / "prism-app-host";
    config.apps_root = bin.parent_path() / "share/prism/apps";
    config.themes_root = bin.parent_path() / "share/prism/themes";
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--help") {
            std::cout << "Usage: prism-launcher [--apps-root DIRECTORY] [--host EXECUTABLE]\n"
                " [--pool-size 0..32] [--max-workers 1..32] [--startup-timeout-ms 100..60000]\n"
                " [--socket BASENAME] [--wayland SOCKET] [--themes-root DIRECTORY] [--theme ID] [--color-scheme dark|light]\n"; return 0;
        }
        if (arg=="--start-shell") { config.start_shell=true; continue; }
        if (++i >= argc) return 2;
        std::string_view value(argv[i]);
        if (arg == "--apps-root") config.apps_root = value;
        else if (arg == "--themes-root") config.themes_root = value;
        else if (arg == "--theme") config.theme_id = value;
        else if (arg == "--color-scheme") config.color_scheme = value;
        else if (arg == "--host") config.host = value;
        else if (arg == "--socket") config.socket_name = value;
        else if (arg == "--wayland") config.wayland = value;
        else {
            unsigned number{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
            if (error != std::errc{} || end != value.data() + value.size()) return 2;
            if (arg == "--wm-fd" && number>0) config.wm_fd=number;
            else if (arg == "--parent-pid" && number>0) config.parent_pid=number;
            else if (arg == "--pool-size" && number <= 32) config.pool_size = number;
            else if (arg == "--max-workers" && number >= 1 && number <= 32) config.max_workers = number;
            else if (arg == "--startup-timeout-ms" && number >= 100 && number <= 60000) config.startup_timeout_ms = number;
            else return 2;
        }
    }
    if (config.parent_pid && (prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()!=config.parent_pid)) return 1;
    std::signal(SIGTERM, Stop); std::signal(SIGINT, Stop); std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGCHLD, SIG_DFL); // The service owns worker waitpid/reaping.
    try { prism::launcher::Service service(std::move(config)); return service.Run(stopping); }
    catch (const std::exception& error) { std::cerr << "Launcher failure: " << error.what() << '\n'; return 1; }
}
