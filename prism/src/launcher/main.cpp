#include "prism/launcher/zygote_server.hpp"
#include "prism/core/logging.hpp"

int main() {
    PRISM_LOG_INFO("LAUNCHER-MAIN", "=================================================");
    PRISM_LOG_INFO("LAUNCHER-MAIN", "       Prism Launcher (Zygote Daemon)            ");
    PRISM_LOG_INFO("LAUNCHER-MAIN", "=================================================");

    prism::launcher::ZygoteServer server;
    if (!server.Start()) {
        return 1;
    }
    return 0;
}
