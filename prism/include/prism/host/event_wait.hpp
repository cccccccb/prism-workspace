#pragma once
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace prism::host {
inline std::uint64_t After(std::uint64_t now,std::uint64_t delay) {
    return delay>std::numeric_limits<std::uint64_t>::max()-now?
        std::numeric_limits<std::uint64_t>::max():now+delay;
}
inline void Earlier(std::optional<std::uint64_t>& deadline,std::uint64_t candidate) {
    if(!deadline||candidate<*deadline)deadline=candidate;
}
// A negative caller limit means infinite. Round up: a sub-millisecond future
// deadline must not become an immediate, repeatedly polled timeout.
inline int Timeout(std::uint64_t now,std::optional<std::uint64_t> deadline,int limit=-1) {
    if(!deadline)return limit<0?-1:limit;
    const auto remaining=*deadline>now?*deadline-now:0;
    const auto milliseconds=remaining/1000000+(remaining%1000000!=0);
    const int duration=static_cast<int>(std::min<std::uint64_t>(milliseconds,std::numeric_limits<int>::max()));
    return limit<0?duration:std::min(limit,duration);
}
inline void Drain(int fd) {
    char bytes[128];
    for(;;){const auto count=read(fd,bytes,sizeof(bytes));
        if(count>0)continue;
        if(count<0&&errno==EINTR)continue;
        break;
    }
}
// One instance per process, constructed before resource threads. Signal
// handlers write only a byte and sig_atomic_t; delivery to another thread or
// the check-before-poll race still leaves a readable notification for the loop.
class SignalWake {
public:
    explicit SignalWake(bool children=false):count_(children?3:2) {
        if(handler_fd_>=0)throw std::logic_error("SignalWake already installed");
        if(pipe2(pipe_,O_NONBLOCK|O_CLOEXEC))throw std::runtime_error("Cannot create signal wake pipe");
        stopping_=0;handler_fd_=pipe_[1];
        struct sigaction action{};action.sa_handler=Handle;sigemptyset(&action.sa_mask);
        for(int i=0;i<count_;++i){
            if(sigaction(signals_[i],&action,&previous_[i])){
                Restore();close(pipe_[0]);close(pipe_[1]);pipe_[0]=pipe_[1]=-1;
                throw std::runtime_error("Cannot install signal wake handler");
            }
            ++installed_;
        }
    }
    ~SignalWake(){Restore();if(pipe_[0]>=0)close(pipe_[0]);if(pipe_[1]>=0)close(pipe_[1]);}
    SignalWake(const SignalWake&)=delete;
    SignalWake& operator=(const SignalWake&)=delete;
    int Fd()const{return pipe_[0];}
    const volatile std::sig_atomic_t& Stopping()const{return stopping_;}
    void RequestStop(){stopping_=1;Notify();}
    void Consume()const{Drain(pipe_[0]);}
private:
    static void Notify() {
        const int fd=handler_fd_;
        if(fd>=0){const char byte=1;ssize_t written;
            do{written=write(fd,&byte,1);}while(written<0&&errno==EINTR);
        }
    }
    static void Handle(int signal) {
        const int saved=errno;
        if(signal==SIGTERM||signal==SIGINT)stopping_=1;
        Notify();errno=saved;
    }
    void Restore(){handler_fd_=-1;for(int i=installed_-1;i>=0;--i)sigaction(signals_[i],&previous_[i],nullptr);installed_=0;}
    inline static volatile std::sig_atomic_t handler_fd_{-1},stopping_{};
    int pipe_[2]{-1,-1},count_{},installed_{};
    const int signals_[3]{SIGTERM,SIGINT,SIGCHLD};
    struct sigaction previous_[3]{};
};
} // namespace prism::host
