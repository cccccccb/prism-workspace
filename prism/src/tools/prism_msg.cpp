#include <iostream>
#include "prism/sdk/launch_client.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
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
              << "  -s <path>      Specify endpoint path (theme commands use the launcher socket) (default: $PRISMSOCK or $XDG_RUNTIME_DIR/prism-ipc.sock)\n"
              << "  -r, --raw      Output raw JSON response without formatting\n"
              << "  -h, --help     Show this help message\n\n"
              << "Commands:\n"
              << "  get_outputs, outputs            Get connected outputs, resolutions, and refresh rates\n"
              << "  set_mode <output> <w> <h> [hz]  Dynamically change output resolution and refresh rate\n"
              << "  get_status, status              Query compositor status, live FPS, and window metrics\n"
              << "  focus <left|right|up|down>      Geometric closest-neighbour focus navigation\n"
              << "  swap <left|right|up|down>       Geometric neighbour window swap\n"
              << "  workspace, ws [name]            Switch or query dynamic workspaces (1..N)\n"
              << "  layout <splith|splitv|tabbed|stacked|overview|split> Change container layout mode\n"
              << "  tree, get_tree                  Dump multi-level recursive container tree in JSON (Swaymsg-like)\n"
              << "  set_theme <id>                 Apply a DSL theme through the unified runtime\n"
              << "  set_color_scheme <light|dark>   Apply a global color palette, keeping the theme\n"
              << "  get_theme                       Query the applied theme and generation\n"
              << "  fold [window_index]             Trigger smooth kinetic fold/unfold on tile (roll-up)\n"
              << "  fullscreen, monocle [win_index] Toggle kinetic fullscreen expansion / restore\n"
              << "  action <app_id> <action_name>   Send action event to application (e.g. player:toggle)\n"
              << "  ipc_test                        Run zero-copy shared memory IPC latency benchmark\n\n"
              << "Examples:\n"
              << "  " << prog << " get_outputs\n"
              << "  " << prog << " focus right\n"
              << "  " << prog << " swap left\n"
              << "  " << prog << " workspace 2\n"
              << "  " << prog << " layout tabbed\n"
              << "  " << prog << " tree\n"
              << "  " << prog << " fold 0\n"
              << "  " << prog << " fullscreen\n"
              << "  " << prog << " set_theme glass\n";
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

    if (cmd_args.front()=="set_theme" || cmd_args.front()=="theme" || cmd_args.front()=="get_theme" || cmd_args.front()=="set_color_scheme") {
        const bool query=cmd_args.front()=="get_theme" || (cmd_args.front()=="theme" && cmd_args.size()==1);
        if(cmd_args.size()!=(query?1u:2u)){PrintUsage(argv[0]);return 2;}
        prism::sdk::LaunchClient client(custom_sock);
        const bool scheme=cmd_args.front()=="set_color_scheme";
        const auto request=client.SelectTheme(query||scheme?std::string{}:cmd_args[1],scheme?cmd_args[1]:std::string{});
        if(!request){std::cerr<<"Cannot connect to the theme owner (launcher socket)\n";return 1;}
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
        while(client.Connected()&&std::chrono::steady_clock::now()<deadline){
            client.Pump(100);
            for(auto& event:client.TakeThemeEvents())if(event.request==request){
                using prism::contracts::ThemeStatus;
                const auto status=event.status==ThemeStatus::Rejected?"rejected":event.status==ThemeStatus::Current?"current":"applied";
                std::cout<<nlohmann::json{{"status",status},{"generation",event.generation},{"id",event.id},{"name",event.name},{"color_scheme",event.color_scheme},{"detail",event.detail}}.dump()<<'\n';
                return event.status==ThemeStatus::Rejected?1:0;
            }
        }
        std::cerr<<"Theme acknowledgement timed out or connection closed\n";return 1;
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
