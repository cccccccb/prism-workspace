#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static void PrintUsage(const char* prog) {
    std::cout << "PrismWM IPC Controller (Swaymsg-like architecture)\n"
              << "Usage: " << prog << " [options] <command> [args...]\n\n"
              << "Options:\n"
              << "  -s <path>      Specify IPC socket path (default: $PRISMSOCK or $XDG_RUNTIME_DIR/prism-ipc.sock)\n"
              << "  -r, --raw      Output raw JSON response without formatting\n"
              << "  -h, --help     Show this help message\n\n"
              << "Commands:\n"
              << "  get_outputs, outputs            Get connected outputs, resolutions, and refresh rates\n"
              << "  set_mode <output> <w> <h> [hz]  Dynamically change output resolution and refresh rate\n"
              << "  get_status, status              Query compositor status, live FPS, and window metrics\n"
              << "  set_layout <split|overview>     Dynamically change window layout or toggle Mission Control\n"
              << "  set_theme <path|nordic|default> Hot-reload or switch Tiling Decoration Theme (.prismb)\n"
              << "  action <app_id> <action_name>   Send action event to application (e.g. player:toggle)\n"
              << "  ipc_test                        Run zero-copy shared memory IPC latency benchmark\n\n"
              << "Examples:\n"
              << "  " << prog << " get_outputs\n"
              << "  " << prog << " set_mode WL-1 1600 900 60\n"
              << "  " << prog << " get_status\n"
              << "  " << prog << " set_theme nordic\n"
              << "  " << prog << " set_theme themes/nordic_glass.prismb\n"
              << "  " << prog << " ipc_test\n";
}

static std::string FindSocketPath(const std::string& custom_sock) {
    if (!custom_sock.empty()) return custom_sock;

    const char* env_sock = getenv("PRISMSOCK");
    if (env_sock && access(env_sock, F_OK) == 0) {
        return env_sock;
    }

    const char* runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir) {
        std::string p = std::string(runtime_dir) + "/prism-ipc.sock";
        if (access(p.c_str(), F_OK) == 0) return p;
    }

    std::string tmp_sock = "/tmp/prism-ipc-" + std::to_string(getuid()) + ".sock";
    if (access(tmp_sock.c_str(), F_OK) == 0) return tmp_sock;

    // Default fallback
    if (runtime_dir) return std::string(runtime_dir) + "/prism-ipc.sock";
    return tmp_sock;
}

static std::string SendIpcCommand(const std::string& sock_path, const std::string& cmd_line) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return "{\"status\": \"error\", \"message\": \"Failed to create socket: " + std::string(strerror(errno)) + "\"}";
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = strerror(errno);
        close(fd);
        return "{\"status\": \"error\", \"message\": \"Failed to connect to PrismWM IPC socket '" + sock_path + "': " + err + "\"}";
    }

    std::string to_send = cmd_line + "\n";
    ssize_t written = write(fd, to_send.data(), to_send.size());
    if (written < 0) {
        close(fd);
        return "{\"status\": \"error\", \"message\": \"Failed to write to socket\"}";
    }

    std::string response;
    char buf[2048];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        response.append(buf, n);
    }
    close(fd);
    return response;
}

int main(int argc, char* argv[]) {
    std::string custom_sock;
    bool raw_output = false;
    std::vector<std::string> cmd_args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s" && i + 1 < argc) {
            custom_sock = argv[++i];
        } else if (arg == "-r" || arg == "--raw") {
            raw_output = true;
            (void)raw_output;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            cmd_args.push_back(arg);
        }
    }

    if (cmd_args.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string sock_path = FindSocketPath(custom_sock);

    std::ostringstream ss;
    for (size_t i = 0; i < cmd_args.size(); ++i) {
        if (i > 0) ss << " ";
        ss << cmd_args[i];
    }
    std::string cmd_line = ss.str();

    std::string response = SendIpcCommand(sock_path, cmd_line);

    // If raw requested or JSON parse not needed, print as-is
    std::cout << response;
    if (response.empty() || response.back() != '\n') {
        std::cout << "\n";
    }

    return 0;
}
