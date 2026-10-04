#include "prism/launch/control_protocol.hpp"
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

namespace prism::launch {
namespace {
void Check(bool ok)
{
    if (!ok) {
        throw std::invalid_argument("Invalid WM control frame");
    }
}

void Put(std::vector<std::uint8_t> &b, std::uint64_t v, unsigned n)
{
    for (unsigned i = n; i; --i) {
        b.push_back(v >> ((i - 1) * 8));
    }
}

struct Reader {
    std::span<const std::uint8_t> b;
    std::size_t at{};

    std::uint64_t Get(unsigned n)
    {
        Check(n <= b.size() - at);
        std::uint64_t v = 0;
        while (n--) {
            v = (v << 8) | b[at++];
        }
        return v;
    }
};
} // namespace

std::uint64_t MonotonicNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void RandomBytes(std::span<std::uint8_t> bytes)
{
    while (!bytes.empty()) {
        auto n = getrandom(bytes.data(), bytes.size(), 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            throw std::runtime_error("getrandom failed");
        }
        bytes = bytes.subspan(n);
    }
}

void VerifyControlPeer(int fd, int parent_pid)
{
    ucred peer{};
    socklen_t n = sizeof(peer);
    int type{};
    socklen_t t = sizeof(type);
    if (parent_pid <= 0 || parent_pid != getppid() ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &n) || peer.pid != parent_pid ||
        peer.uid != geteuid() || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &t) ||
        type != SOCK_STREAM) {
        throw std::runtime_error("WM control endpoint is not from the session supervisor");
    }
}

std::vector<std::uint8_t> EncodeControl(const ControlMessage &m)
{
    Check(static_cast<unsigned>(m.type) >= 1 && static_cast<unsigned>(m.type) <= 14);
    Check(m.permit.session && static_cast<unsigned>(m.permit.role) <=
                                  static_cast<unsigned>(contracts::WindowRole::LayoutControls));
    std::vector<std::uint8_t> body;
    const auto &p = m.permit;
    Put(body, p.session, 8);
    if (m.type == ControlType::LayoutSubscribe) {
        Put(body, m.layout_subscribe, 1);
    } else if (m.type == ControlType::LayoutSnapshot) {
        Check(m.layout_snapshot.session == p.session);
        auto payload = contracts::EncodeLayoutSnapshot(m.layout_snapshot);
        body.insert(body.end(), payload.begin(), payload.end());
    } else if (m.type == ControlType::InstallTheme || m.type == ControlType::ThemeApplied) {
        if (m.type == ControlType::InstallTheme) {
            Check(m.theme.generation);
        }
        auto payload = m.type == ControlType::InstallTheme
                           ? contracts::EncodeTheme(m.theme)
                           : contracts::EncodeThemeApplied(m.theme_applied);
        body.insert(body.end(), payload.begin(), payload.end());
    } else {
        Put(body, p.request.value, 8);
        Put(body, p.instance.value, 8);
        Put(body, p.pid, 4);
        Put(body, static_cast<unsigned>(p.role), 1);
        body.insert(body.end(), p.token.begin(), p.token.end());
        Put(body, p.expires_ns, 8);
        Put(body, m.success, 1);
        if (m.type == ControlType::LayoutControl || m.type == ControlType::LayoutControlResult) {
            Check(p.instance.value && p.pid);
            auto payload = m.type == ControlType::LayoutControl
                               ? contracts::EncodeLayoutControl(m.control_request)
                               : contracts::EncodeLayoutControlResult(m.control_result);
            body.insert(body.end(), payload.begin(), payload.end());
        }
    }
    std::vector<std::uint8_t> b;
    Put(b, 0x50574331, 4);
    Put(b, 1, 2);
    Put(b, static_cast<unsigned>(m.type), 2);
    Put(b, body.size(), 4);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

std::size_t ControlFrameSize(std::span<const std::uint8_t> b)
{
    if (b.size() < 12) {
        return 0;
    }
    Reader r{b.first(12)};
    Check(r.Get(4) == 0x50574331 && r.Get(2) == 1);
    auto t = r.Get(2), n = r.Get(4);
    Check(t >= 1 && t <= 14);
    if (t <= 8) {
        Check(n == 70);
    } else if (t <= 10) {
        Check(n >= 8 && n <= contracts::kMaxThemePayload + 8);
    } else if (t == 11) {
        Check(n == 9);
    } else if (t == 12) {
        Check(n >= 8 && n <= contracts::kMaxLayoutSnapshotPayload + 8);
    } else {
        Check((t == 13 && n == 70 + contracts::kWindowControlPayload) ||
              n == 70 + (t == 13 ? contracts::kLayoutControlPayload
                                 : contracts::kLayoutControlResultPayload));
    }
    return 12 + n;
}

ControlMessage DecodeControl(std::span<const std::uint8_t> b)
{
    Check(ControlFrameSize(b) == b.size());
    Reader h{b};
    h.Get(4);
    h.Get(2);
    ControlMessage m;
    m.type = static_cast<ControlType>(h.Get(2));
    h.Get(4);
    auto &p = m.permit;
    p.session = h.Get(8);
    Check(p.session);
    if (m.type == ControlType::LayoutSubscribe) {
        const auto enabled = h.Get(1);
        Check(enabled <= 1);
        m.layout_subscribe = enabled;
        return m;
    }
    if (m.type == ControlType::LayoutSnapshot) {
        m.layout_snapshot = contracts::DecodeLayoutSnapshot(b.subspan(20));
        Check(m.layout_snapshot.session == p.session);
        return m;
    }
    if (m.type == ControlType::InstallTheme) {
        m.theme = contracts::DecodeTheme(b.subspan(20));
        Check(m.theme.generation);
        return m;
    }
    if (m.type == ControlType::ThemeApplied) {
        m.theme_applied = contracts::DecodeThemeApplied(b.subspan(20));
        return m;
    }
    p.request = {h.Get(8)};
    p.instance = {h.Get(8)};
    p.pid = h.Get(4);
    auto role = h.Get(1);
    Check(role <= static_cast<unsigned>(contracts::WindowRole::LayoutControls));
    p.role = static_cast<contracts::WindowRole>(role);
    for (auto &v : p.token) {
        v = h.Get(1);
    }
    p.expires_ns = h.Get(8);
    auto success = h.Get(1);
    Check(success <= 1);
    m.success = success;
    if (m.type == ControlType::LayoutControl || m.type == ControlType::LayoutControlResult) {
        Check(p.instance.value && p.pid);
        if (m.type == ControlType::LayoutControl) {
            m.control_request = contracts::DecodeLayoutControl(b.subspan(h.at));
        } else {
            m.control_result = contracts::DecodeLayoutControlResult(b.subspan(h.at));
        }
    }
    return m;
}

void LayoutSnapshotCache::Reset(std::uint64_t session)
{
    session_ = session;
    current_.reset();
}

void LayoutSnapshotCache::Accept(contracts::LayoutSnapshot snapshot)
{
    Check(session_ && snapshot.session == session_);
    contracts::ValidateLayoutSnapshot(snapshot);
    if (current_) {
        Check(snapshot.revision > current_->revision &&
              snapshot.topology_revision >= current_->topology_revision &&
              snapshot.layout_revision >= current_->layout_revision &&
              snapshot.focus_revision >= current_->focus_revision);
        for (const auto &workspace : snapshot.workspaces) {
            for (const auto &previous : current_->workspaces) {
                if (workspace.id == previous.id) {
                    Check(workspace.mode_revision >= previous.mode_revision);
                }
            }
        }
    }

    current_ = std::make_shared<const contracts::LayoutSnapshot>(std::move(snapshot));
}

std::shared_ptr<const contracts::LayoutSnapshot> LayoutSnapshotCache::Current() const noexcept
{
    return current_;
}
} // namespace prism::launch
