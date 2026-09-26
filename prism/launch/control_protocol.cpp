#include "prism/launch/control_protocol.hpp"
#include <chrono>
#include <cerrno>
#include <stdexcept>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>
namespace prism::launch {
namespace {
void Check(bool ok) { if (!ok) throw std::invalid_argument("Invalid WM control frame"); }
void Put(std::vector<std::uint8_t>& b, std::uint64_t v, unsigned n) {
    for (unsigned i=n; i; --i) b.push_back(v >> ((i-1)*8));
}
struct Reader {
    std::span<const std::uint8_t> b; std::size_t at{};
    std::uint64_t Get(unsigned n) {
        Check(n <= b.size()-at); std::uint64_t v=0;
        while (n--) v=(v<<8)|b[at++];
        return v;
    }
};
}
std::uint64_t MonotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void RandomBytes(std::span<std::uint8_t> bytes) {
    while (!bytes.empty()) {
        auto n=getrandom(bytes.data(), bytes.size(), 0);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) throw std::runtime_error("getrandom failed");
        bytes=bytes.subspan(n);
    }
}
void VerifyControlPeer(int fd, int parent_pid) {
    ucred peer{}; socklen_t n=sizeof(peer); int type{}; socklen_t t=sizeof(type);
    if (parent_pid<=0 || parent_pid!=getppid() ||
        getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&n) || peer.pid!=parent_pid || peer.uid!=geteuid() ||
        getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&t) || type!=SOCK_STREAM)
        throw std::runtime_error("WM control endpoint is not from the session supervisor");
}
std::vector<std::uint8_t> EncodeControl(const ControlMessage& m) {
    Check(static_cast<unsigned>(m.type)>=1 && static_cast<unsigned>(m.type)<=8);
    Check(m.permit.session && static_cast<unsigned>(m.permit.role)<=3);
    std::vector<std::uint8_t> b;
    Put(b,0x50574331,4); Put(b,1,2); Put(b,static_cast<unsigned>(m.type),2); Put(b,70,4);
    const auto& p=m.permit;
    Put(b,p.session,8); Put(b,p.request.value,8); Put(b,p.instance.value,8); Put(b,p.pid,4);
    Put(b,static_cast<unsigned>(p.role),1);
    b.insert(b.end(),p.token.begin(),p.token.end()); Put(b,p.expires_ns,8); Put(b,m.success,1);
    return b;
}
std::size_t ControlFrameSize(std::span<const std::uint8_t> b) {
    if (b.size()<12) return 0;
    Reader r{b.first(12)}; Check(r.Get(4)==0x50574331 && r.Get(2)==1);
    auto t=r.Get(2); Check(t>=1 && t<=8); Check(r.Get(4)==70); return 82;
}
ControlMessage DecodeControl(std::span<const std::uint8_t> b) {
    Check(ControlFrameSize(b)==b.size()); Reader h{b}; h.Get(4); h.Get(2);
    ControlMessage m; m.type=static_cast<ControlType>(h.Get(2)); h.Get(4);
    auto& p=m.permit; p.session=h.Get(8); p.request={h.Get(8)}; p.instance={h.Get(8)};
    p.pid=h.Get(4); auto role=h.Get(1); Check(role<=3); p.role=static_cast<contracts::WindowRole>(role);
    for (auto& v:p.token) v=h.Get(1);
    p.expires_ns=h.Get(8);
    auto success=h.Get(1); Check(success<=1 && p.session); m.success=success; return m;
}
} // namespace prism::launch
