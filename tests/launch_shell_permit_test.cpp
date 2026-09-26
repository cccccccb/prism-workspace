#include "prism/launch/shell_permit.hpp"
#include <cassert>
#include <stdexcept>
using namespace prism::launch;
int main() {
    ShellPermit permit{1, {2}, {3}, 42, prism::contracts::WindowRole::Dock, {}, 1000};
    permit.token[0] = 17;
    for (int change = 0; change < 7; ++change) {
        ShellPermitGuard guard(permit);
        auto bad = permit;
        if (change == 0) ++bad.session;
        if (change == 1) ++bad.request.value;
        if (change == 2) ++bad.instance.value;
        if (change == 3) ++bad.pid;
        if (change == 4) bad.role = prism::contracts::WindowRole::Desktop;
        if (change == 5) ++bad.token[0];
        if (change == 6) ++bad.expires_ns;
        assert(!guard.Consume(bad, 999));
        assert(guard.Consume(permit, 999));
        assert(!guard.Consume(permit, 999));
    }
    ShellPermitGuard expired(permit);
    assert(!expired.Consume(permit, 1000));
    ShellPermitGuard revoked(permit); revoked.Revoke();
    assert(!revoked.Consume(permit, 1));
    permit.role = prism::contracts::WindowRole::Toplevel;
    bool rejected = false;
    try { ShellPermitGuard invalid(permit); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
