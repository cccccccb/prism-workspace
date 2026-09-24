#include "prism/invoker/launch_pipeline.hpp"
#include "prism/launcher/zygote_server.hpp"
#include "prism/core/logging.hpp"
#include "prism/core/types.hpp"
#include <unistd.h>

int main(int argc, char* argv[]) {
    PRISM_LOG_INFO("INVOKER-MAIN", "=================================================");
    PRISM_LOG_INFO("INVOKER-MAIN", "         Prism App Launch Invoker                ");
    PRISM_LOG_INFO("INVOKER-MAIN", "=================================================");

    std::string app = (argc > 1) ? argv[1] : "demo_player";

    prism::invoker::LaunchContext ctx;
    if (app.size() >= 9 && app.rfind(".prismpkg") == app.size() - 9) {
        ctx.package_path = app;
        ctx.app_name = app.substr(0, app.rfind(".prismpkg"));
        size_t slash = ctx.app_name.find_last_of('/');
        if (slash != std::string::npos) ctx.app_name = ctx.app_name.substr(slash + 1);
    } else {
        ctx.app_name = app;
    }
    ctx.channel_name = "/prism_" + ctx.app_name + "_" + std::to_string(getpid());
    ctx.zygote_socket = prism::launcher::DEFAULT_ZYGOTE_SOCKET;

    // Build Chain of Responsibility pipeline
    auto manifest_step = std::make_shared<prism::invoker::ManifestValidationStep>();
    auto sandbox_step  = std::make_shared<prism::invoker::SandboxSecurityStep>();
    auto preview_step  = std::make_shared<prism::invoker::PreviewMountStep>();
    auto zygote_step   = std::make_shared<prism::invoker::ZygoteDispatchStep>();

    manifest_step->SetNext(sandbox_step)
                 ->SetNext(preview_step)
                 ->SetNext(zygote_step);

    auto t_start = prism::core::CurrentTimeNs();
    bool success = manifest_step->Handle(ctx);
    auto elapsed_us = (prism::core::CurrentTimeNs() - t_start) / 1000.0;

    if (success) {
        PRISM_LOG_INFO("INVOKER-MAIN", "Launch pipeline succeeded in %.2f us (%.3f ms)", elapsed_us, elapsed_us / 1000.0);
        return 0;
    } else {
        PRISM_LOG_ERROR("INVOKER-MAIN", "Launch pipeline aborted due to error");
        return 1;
    }
}
