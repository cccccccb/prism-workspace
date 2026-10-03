#include "prism/launch/shell_permit.hpp"
#include <algorithm>
#include <stdexcept>

namespace prism::launch {
ShellPermitGuard::ShellPermitGuard(ShellPermit permit) : permit_(permit)
{
    using contracts::WindowRole;
    if (!permit.session || !permit.request.value || !permit.instance.value || !permit.pid ||
        !permit.expires_ns ||
        (permit.role != WindowRole::Desktop && permit.role != WindowRole::TopBar &&
         permit.role != WindowRole::Dock && permit.role != WindowRole::LayoutControls) ||
        std::all_of(permit.token.begin(), permit.token.end(),
                    [](auto byte) { return byte == 0; })) {
        throw std::invalid_argument("Invalid Shell permit");
    }
}

bool ShellPermitGuard::Consume(const ShellPermit &claim, std::uint64_t now_ns)
{
    if (consumed_ || now_ns >= permit_.expires_ns || claim.session != permit_.session ||
        claim.request != permit_.request || claim.instance != permit_.instance ||
        claim.pid != permit_.pid || claim.role != permit_.role ||
        claim.expires_ns != permit_.expires_ns) {
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < claim.token.size(); ++i) {
        difference |= claim.token[i] ^ permit_.token[i];
    }
    if (difference) {
        return false;
    }
    consumed_ = true;
    return true;
}
} // namespace prism::launch
