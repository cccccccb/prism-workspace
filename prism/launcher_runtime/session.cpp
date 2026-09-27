#include "prism/host/event_wait.hpp"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
extern char **environ;

namespace {
std::uint64_t Now()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::vector<pid_t> Children()
{
    const auto pid = std::to_string(getpid());
    std::ifstream input("/proc/self/task/" + pid + "/children");
    if (!input) {
        throw std::runtime_error("Cannot inspect session child ownership");
    }
    std::vector<pid_t> children;
    pid_t child{};
    while (input >> child) {
        children.push_back(child);
    }
    return children;
}

pid_t Spawn(const std::filesystem::path &executable, int endpoint, int other,
            std::vector<std::string> arguments)
{
    arguments.insert(arguments.begin(), executable.string());
    std::vector<char *> words;
    for (auto &word : arguments) {
        words.push_back(word.data());
    }
    words.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, endpoint, 3);
    if (endpoint != 3) {
        posix_spawn_file_actions_addclose(&actions, endpoint);
    }
    if (other != 3) {
        posix_spawn_file_actions_addclose(&actions, other);
    }
    pid_t pid{};
    int error = posix_spawn(&pid, executable.c_str(), &actions, nullptr, words.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error) {
        throw std::runtime_error("Session spawn failed: " + executable.string() +
                                 " error=" + std::to_string(error));
    }
    return pid;
}
} // namespace

int main(int argc, char **argv)
{
    auto bin = std::filesystem::canonical("/proc/self/exe").parent_path();
    auto wm = bin / "prism-wm", launcher = bin / "prism-launcher",
         root = bin.parent_path() / "share/prism/apps";
    auto themes = bin.parent_path() / "share/prism/themes";
    std::string theme_id = "glass", color_scheme = "dark";
    for (int i = 1; i < argc; ++i) {
        std::string_view option(argv[i]);
        if (++i >= argc) {
            return 2;
        }
        if (option == "--wm") {
            wm = argv[i];
        } else if (option == "--launcher") {
            launcher = argv[i];
        } else if (option == "--apps-root") {
            root = argv[i];
        } else if (option == "--themes-root") {
            themes = argv[i];
        } else if (option == "--theme") {
            theme_id = argv[i];
        } else if (option == "--color-scheme") {
            color_scheme = argv[i];
        } else {
            return 2;
        }
    }
    prism::host::SignalWake signals(true);
    const auto &stopping = signals.Stopping();
    // PAM/systemd can leave an existing sd-pam helper across exec. It ends
    // after its parent exits; waiting for it would deadlock session shutdown.
    const auto existing = Children();
    const std::set<pid_t> inherited(existing.begin(), existing.end());
    if (prctl(PR_SET_CHILD_SUBREAPER, 1)) {
        return 1;
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) {
        return 1;
    }
    pid_t wm_pid{}, launcher_pid{};
    int result = 0;
    const auto parent = std::to_string(getpid());
    try {
        wm_pid = Spawn(wm, pair[0], pair[1], {"--control-fd", "3", "--parent-pid", parent});
        launcher_pid =
            Spawn(launcher, pair[1], pair[0],
                  {"--wm-fd", "3", "--parent-pid", parent, "--start-shell", "--apps-root",
                   root.string(), "--themes-root", themes.string(), "--theme", theme_id,
                   "--color-scheme", color_scheme, "--wayland", "wayland-prism-0"});
        std::cout << "session wm=" << wm_pid << " launcher=" << launcher_pid << std::endl;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        signals.RequestStop();
        result = 1;
    }
    close(pair[0]);
    close(pair[1]);
    while (!stopping) {
        for (auto *principal : {&wm_pid, &launcher_pid}) {
            if (!*principal) {
                continue;
            }
            int status{};
            const auto reaped = waitpid(*principal, &status, WNOHANG);
            if (reaped == *principal) {
                *principal = 0;
                result = 1;
                signals.RequestStop();
            } else if (reaped < 0 && errno != EINTR) {
                // Ownership was lost: never signal a possibly reused PID.
                *principal = 0;
                result = 1;
                signals.RequestStop();
            }
        }
        if (!stopping) {
            pollfd wake{signals.Fd(), POLLIN, 0};
            const int waited = poll(&wake, 1, -1);
            if (waited < 0 && errno != EINTR) {
                result = 1;
                signals.RequestStop();
            }
            if (wake.revents) {
                signals.Consume();
            }
        }
    }
    if (wm_pid) {
        kill(wm_pid, SIGTERM);
    }
    if (launcher_pid) {
        kill(launcher_pid, SIGTERM);
    }
    const auto deadline = prism::host::After(Now(), 3000000000ULL);
    bool escalated = false;
    for (;;) {
        bool remaining = false, rescan = false;
        for (const auto child : Children()) {
            if (inherited.contains(child)) {
                continue;
            }
            // Re-scan once after reaping a principal: it may just have
            // transferred additional descendants to this subreaper.
            remaining = true;
            int status{};
            const auto reaped = waitpid(child, &status, WNOHANG);
            if (reaped == child) {
                rescan = true;
                if (child == wm_pid) {
                    wm_pid = 0;
                }
                if (child == launcher_pid) {
                    launcher_pid = 0;
                }
            } else if (reaped == 0) {
                remaining = true;
                // Includes adopted hosts/descendants after a launcher crash.
                if (Now() >= deadline) {
                    kill(child, SIGKILL);
                }
            } else if (errno != ECHILD && errno != EINTR) {
                result = 1;
            } else if (errno == EINTR) {
                remaining = true;
            } else {
                rescan = true;
            }
        }
        if (!remaining) {
            break;
        }
        // Reaping a principal can expose already exited adopted descendants;
        // no fresh SIGCHLD is required after a coalesced notification was read.
        if (rescan) {
            continue;
        }
        // SIGCHLD covers principal reaping and newly adopted descendants. Once
        // escalation is sent, await reaping rather than polling an expired timer.
        escalated |= Now() >= deadline;
        pollfd wake{signals.Fd(), POLLIN, 0};
        const int waited =
            poll(&wake, 1,
                 prism::host::Timeout(Now(), escalated ? std::nullopt : std::optional{deadline}));
        if (waited < 0 && errno != EINTR) {
            result = 1;
            break;
        }
        if (wake.revents) {
            signals.Consume();
        }
    }
    return result;
}
