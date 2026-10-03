#include "prism/launch/control_protocol.hpp"
#include "prism/launch/instance_state.hpp"
#include "prism/launch/protocol.hpp"
#include <cassert>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace prism;

int main()
{
    launch::ControlMessage grant;
    grant.type = launch::ControlType::Grant;
    grant.permit = {
        99, {4}, {7}, 42, contracts::WindowRole::Dock, {}, launch::MonotonicNs() + 1000000000ULL};
    launch::RandomBytes(grant.permit.token);
    auto bytes = launch::EncodeControl(grant);
    assert(bytes.size() == 82 && bytes[0] == 'P' && bytes[1] == 'W');
    auto decoded = launch::DecodeControl(bytes);
    assert(decoded.permit.token == grant.permit.token && decoded.permit.pid == 42);
    auto bad = bytes;
    bad[11] = 71;
    bool rejected = false;
    try {
        launch::DecodeControl(bad);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    for (unsigned role = 0; role <= 4; ++role) {
        auto role_grant = grant;
        role_grant.permit.role = static_cast<contracts::WindowRole>(role);
        const auto frame = launch::EncodeControl(role_grant);
        assert(frame.size() == bytes.size());
        assert(launch::DecodeControl(frame).permit.role == role_grant.permit.role);
    }
    auto invalid_role = grant;
    invalid_role.permit.role = static_cast<contracts::WindowRole>(5);
    rejected = false;
    try {
        launch::EncodeControl(invalid_role);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    bad = bytes;
    bad[40] = 5;
    rejected = false;
    try {
        launch::DecodeControl(bad);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);

    launch::ShellPermitGuard guard(decoded.permit);
    auto wrong = decoded.permit;
    wrong.instance = {8};
    assert(!guard.Consume(wrong, launch::MonotonicNs()));
    assert(guard.Consume(decoded.permit, launch::MonotonicNs()));
    assert(!guard.Consume(decoded.permit, launch::MonotonicNs()));
    contracts::InstanceUpdate update{
        {10}, {7}, 42, contracts::InstanceChange::Running, "demo_player"};
    auto result =
        std::get<contracts::InstanceUpdate>(launch::DecodeMessage(launch::EncodeMessage(update)));
    assert(result.instance == update.instance && result.app_id == update.app_id);
    auto snapshot = std::get<contracts::InstanceSubscribe>(
        launch::DecodeMessage(launch::EncodeMessage(contracts::InstanceSubscribe{{10}})));
    assert(snapshot.request.value == 10);
    update.instance = {};
    rejected = false;
    try {
        launch::EncodeMessage(update);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    launch::InstanceState activation({4}, {7});
    assert(activation.Apply(
        {{4}, {7}, 0, contracts::LaunchMilestone::Accepted, contracts::LaunchError::None, 0, {}}));
    assert(activation.Apply({{4},
                             {7},
                             42,
                             contracts::LaunchMilestone::WorkerAssigned,
                             contracts::LaunchError::None,
                             0,
                             {}}));
    assert(activation.Apply({{4},
                             {7},
                             42,
                             contracts::LaunchMilestone::Activated,
                             contracts::LaunchError::None,
                             0,
                             {}}));
    assert(activation.Activated() && !activation.FirstPresented() && !activation.BackendReady());
    assert(!activation.Apply({{4},
                              {7},
                              42,
                              contracts::LaunchMilestone::Activated,
                              contracts::LaunchError::None,
                              0,
                              {}}));
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        try {
            launch::VerifyControlPeer(pair[1], getppid());
        } catch (...) {
            _exit(1);
        }
        bool bad_parent = false;
        try {
            launch::VerifyControlPeer(pair[1], getppid() + 1);
        } catch (...) {
            bad_parent = true;
        }
        _exit(bad_parent ? 0 : 1);
    }
    close(pair[0]);
    close(pair[1]);
    int status{};
    waitpid(child, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
