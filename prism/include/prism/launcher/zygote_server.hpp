#pragma once

#include "prism/core/noncopyable.hpp"
#include <string>

namespace prism::launcher {

constexpr const char* DEFAULT_ZYGOTE_SOCKET = "/tmp/prism_zygote.sock";

/**
 * @brief Prototype / Process Factory Pattern: Pre-warms runtime memory and forks backend processes instantaneously
 */
class ZygoteServer : public core::NonCopyable {
public:
    ZygoteServer();
    ~ZygoteServer();

    bool Start(const std::string& socket_path = DEFAULT_ZYGOTE_SOCKET);
    void Stop();

    void Prewarm();

private:
    void RunLoop();
    void HandleClient(int client_fd);

    std::string socket_path_;
    int server_fd_{-1};
    bool running_{false};
};

} // namespace prism::launcher
