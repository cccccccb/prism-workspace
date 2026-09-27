#include "prism/host/event_wait.hpp"
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <pthread.h>

int main() {
    prism::host::SignalWake signals(true);
    pollfd source{signals.Fd(), POLLIN, 0};
    assert(poll(&source, 1, 0) == 0 && !signals.Stopping());
    // A notification before entering poll survives the check/wait boundary.
    assert(raise(SIGCHLD) == 0);
    assert(poll(&source, 1, 0) == 1 && (source.revents & POLLIN));
    assert(!signals.Stopping());
    signals.Consume();
    assert(poll(&source, 1, 0) == 0);

    std::mutex mutex;
    std::condition_variable condition;
    bool release = false;
    std::thread resource_thread([&] {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return release; });
    });
    // Process-wide signal disposition must wake the main loop even when the
    // signal runs on the resource thread. No timeout polling is needed.
    assert(pthread_kill(resource_thread.native_handle(), SIGTERM) == 0);
    assert(poll(&source, 1, 1000) == 1 && (source.revents & POLLIN));
    assert(signals.Stopping());
    signals.Consume();
    assert(poll(&source, 1, 0) == 0);
    { std::lock_guard lock(mutex); release = true; }
    condition.notify_one();
    resource_thread.join();

    assert(prism::host::Timeout(100, std::nullopt) == -1);
    assert(prism::host::Timeout(100, 101) == 1);
    assert(prism::host::Timeout(100, 100) == 0);
    assert(prism::host::Timeout(100, 3000001, 2) == 2);
}
