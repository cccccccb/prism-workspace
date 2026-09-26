#include "worker.hpp"
#include "prism/sdk/app_host.hpp"
#include "prism/sdk/module_session.hpp"
#include "prism/launch/stream.hpp"
#include "prism/launch/worker_protocol.hpp"
#include "prism/launch/error.hpp"
#include <iostream>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

int RunWorker(int fd, const std::filesystem::path& apps, const std::string& socket,
              int parent, const volatile std::sig_atomic_t& stopping) {
    using namespace prism;
    ucred credential{}; socklen_t length = sizeof(credential);
    if (parent <= 1 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &length) ||
        credential.pid != parent || credential.uid != geteuid() ||
        prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) return 2;
    launch::Stream channel(fd, launch::WorkerFrameSize);
    std::uint64_t next_launch = 1;
    contracts::RequestId request{};
    contracts::InstanceId instance{};
    sdk::HostConfig config;
    config.socket = socket;
    config.on_event = [&](const contracts::LaunchEvent& event) {
        if (request.value) channel.Queue(launch::EncodeWorker(event));
    };
    config.launch_app = [&](std::string_view app_id) -> std::uint64_t {
        if (!next_launch || channel.Closed()) return 0;
        const auto id = next_launch++;
        return channel.Queue(launch::EncodeWorker(contracts::LaunchRequest{{id}, std::string(app_id)})) ? id : 0;
    };
    config.subscribe_instances=[&]() -> std::uint64_t {
        if (!next_launch || channel.Closed()) return 0;
        auto id=next_launch++;
        return channel.Queue(launch::EncodeWorker(contracts::InstanceSubscribe{{id}})) ? id : 0;
    };
    config.select_theme=[&](std::string_view theme_id) -> std::uint64_t {
        if (!next_launch || channel.Closed()) return 0;
        const auto id=next_launch++;
        return channel.Queue(launch::EncodeWorker(contracts::ThemeRequest{id,std::string(theme_id)})) ? id : 0;
    };
    sdk::AppHost host(std::move(config));
    const auto start = sdk::MonotonicNs();
    if (!host.PrepareFrontend()) return 1;
    channel.Queue(launch::EncodeWorker(launch::WorkerReady{sdk::MonotonicNs() - start}));
    channel.Flush();
    bool bound = false;
    int exit_code = 0;
    try {
        while (!stopping && !channel.Closed()) {
            for (auto& frame : channel.Receive()) {
                const auto message = launch::DecodeWorker(frame);
                if (const auto* snapshot = std::get_if<contracts::ThemeSnapshot>(&message)) {
                    std::string diagnostic;
                    const bool accepted=host.ApplyTheme(*snapshot,&diagnostic);
                    if (accepted) std::cout << "worker theme installed pid=" << getpid()
                        << " generation=" << snapshot->generation << " id=" << snapshot->id
                        << " bound=" << bound << std::endl;
                    if (!channel.Queue(launch::EncodeWorker(contracts::ThemeApplied{
                            snapshot->generation,accepted,std::move(diagnostic)})))
                        throw std::runtime_error("Cannot acknowledge theme update");
                } else if (const auto* bind = std::get_if<launch::WorkerBind>(&message)) {
                    if (bound) throw std::runtime_error("Worker already assigned");
                    if (!host.ThemeGeneration()) throw std::runtime_error("Worker bind requires an acknowledged theme");
                    bound = true; request = bind->request.request; instance = bind->instance;
                    if (!host.Assign(request, instance)) throw std::runtime_error("Invalid worker assignment");
                    auto package = launch::LoadRegisteredPackage(apps, bind->request.app_id);
                    if (!host.Bind(package)) { exit_code = 1; break; }
                } else if (const auto* reply = std::get_if<launch::WorkerReply>(&message)) {
                    if (!bound) throw std::runtime_error("Reply to an idle worker");
                    host.DeliverLaunchEvent(reply->event);
                } else if (const auto* update=std::get_if<contracts::InstanceUpdate>(&message)) {
                    if (!bound) throw std::runtime_error("Instance update to idle worker");
                    host.DeliverInstanceEvent(*update);
                } else if (const auto* event=std::get_if<contracts::ThemeEvent>(&message)) {
                    host.DeliverThemeEvent(*event);
                } else throw std::runtime_error("Unexpected worker command");
            }
            if (exit_code) break;
            if (bound) {
                if (!host.Pump(20)) { exit_code = host.IsCloseRequested() ? 0 : 1; break; }
            } else {
                pollfd wait{channel.Fd(), static_cast<short>(POLLIN | (channel.WantsWrite() ? POLLOUT : 0)), 0};
                poll(&wait, 1, 1000);
            }
            channel.Flush();
        }
    } catch (const launch::LaunchFailure& error) {
        if (request.value) channel.Queue(launch::EncodeWorker(contracts::LaunchEvent{request, instance,
            static_cast<std::uint32_t>(getpid()), contracts::LaunchMilestone::Failed, error.Code(), 0, error.what()}));
        exit_code = 1;
    } catch (const std::exception& error) {
        std::cerr << "Worker failure: " << error.what() << '\n';
        if (request.value) channel.Queue(launch::EncodeWorker(contracts::LaunchEvent{request, instance,
            static_cast<std::uint32_t>(getpid()), contracts::LaunchMilestone::Failed,
            contracts::LaunchError::RuntimeFailed, 0, "Worker control/runtime failed"}));
        exit_code = 1;
    }
    host.Close();
    const auto deadline = sdk::MonotonicNs() + 1000000000ULL;
    while (!channel.Closed() && channel.WantsWrite() && sdk::MonotonicNs() < deadline) {
        channel.Flush();
        pollfd wait{channel.Fd(), POLLOUT, 0}; poll(&wait, 1, 10);
    }
    return exit_code;
}
