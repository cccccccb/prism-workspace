#include "prism/launcher/service.hpp"
#include "prism/theme/compiler.hpp"
#include "prism/host/event_wait.hpp"
#include <optional>
#include <set>
#include "prism/launch/stream.hpp"
#include "prism/launch/worker_protocol.hpp"
#include "prism/launch/instance_state.hpp"
#include "prism/launch/package.hpp"
#include "prism/launch/error.hpp"
#include "prism/launch/control_protocol.hpp"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace prism::launcher {
namespace {
using namespace contracts;
std::uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Require(bool valid, const char* detail) { if (!valid) throw std::runtime_error(detail); }
struct Owner { bool worker{}; std::uint64_t endpoint{}, request{}; };
struct Endpoint {
    std::unique_ptr<launch::Stream> stream;
    std::map<std::uint64_t, std::uint64_t> requests;
    std::uint64_t partial_since{}, subscription{};
    std::map<std::uint64_t, contracts::ThemeEvent> theme_requests;
    std::map<std::uint64_t, std::pair<std::string,std::string>> theme_ids;
};
struct Job {
    Job(std::uint64_t id, LaunchRequest source, Owner endpoint)
        : request(std::move(source)), owner(endpoint), state(request.request, {id}),
          instance{id}, created(Now()) {}
    std::uint64_t alias{};
    WindowRole role{WindowRole::Toplevel};
    bool mapped{}, registered{}, activation_sent{}, bound_sent{};
    LaunchRequest request;
    Owner owner;
    launch::InstanceState state;
    InstanceId instance;
    std::vector<LaunchEvent> history;
    std::uint64_t created;
};
struct Worker : Endpoint {
    enum class Phase { Preparing, Idle, Assigned, Finishing, Stopping };
    pid_t pid{};
    Phase phase{Phase::Preparing};
    std::uint64_t job{}, created{}, finish_at{}, kill_at{}, closed_at{}, theme_generation{};
    bool frontend_ready{};
};
}
struct Service::Impl {
    explicit Impl(ServiceConfig value) : config(std::move(value)) {}
    ServiceConfig config;
    int listener{-1}, lock{-1};
    std::filesystem::path socket_path;
    dev_t socket_device{}; ino_t socket_inode{};
    std::map<std::uint64_t, Endpoint> clients;
    std::map<pid_t, Worker> workers;
    std::map<std::uint64_t, std::unique_ptr<Job>> jobs;
    std::uint64_t next_endpoint{1}, next_job{1}, retry_at{};
    bool shutting_down{}, control_failed{};
    int signal_fd{-1};
    std::unique_ptr<launch::Stream> control;
    std::uint64_t session{}, opened_at{};
    ThemeSnapshot theme, committed_theme;
    bool theme_ready{};
    struct ThemeTransaction {
        Owner owner;
        std::uint64_t request{}, deadline{};
        ThemeSnapshot previous;
        std::set<pid_t> pending;
        bool wm_pending{}, rollback{};
        std::string detail;
    };
    std::optional<ThemeTransaction> theme_transaction;
    ~Impl() {
        Shutdown();
        if (listener >= 0) close(listener);
        struct stat info{};
        if (socket_inode && !lstat(socket_path.c_str(), &info) &&
            info.st_dev == socket_device && info.st_ino == socket_inode) unlink(socket_path.c_str());
        if (lock >= 0) close(lock);
    }
    void Open() {
        opened_at=Now();
        if (config.themes_root.empty()) config.themes_root=prism::theme::DefaultThemeRoot();
        theme=prism::theme::LoadTheme(config.themes_root,config.theme_id,1,config.color_scheme);
        committed_theme=theme; theme_ready=config.wm_fd<0;
        if (config.wm_fd>=0) {
            launch::VerifyControlPeer(config.wm_fd,config.parent_pid);
            control=std::make_unique<launch::Stream>(config.wm_fd,launch::ControlFrameSize);
        }
        Require(!config.start_shell || control, "Shell bootstrap requires trusted WM control");
        Require(config.pool_size <= config.max_workers && config.max_workers >= 1 && config.max_workers <= 32,
            "Pool/max worker counts are invalid");
        struct sigaction child_action{};
        Require(!sigaction(SIGCHLD, nullptr, &child_action) && child_action.sa_handler != SIG_IGN &&
            !(child_action.sa_flags & SA_NOCLDWAIT), "Launcher requires ownership of SIGCHLD/waitpid");
        config.apps_root = std::filesystem::canonical(config.apps_root);
        config.host = std::filesystem::canonical(config.host);
        Require(std::filesystem::is_directory(config.apps_root) && access(config.host.c_str(), X_OK) == 0,
            "Registry or host path is invalid");
        Require(!config.socket_name.empty() && config.socket_name.size() < 64 &&
            config.socket_name.find('/') == std::string::npos && config.socket_name != "." && config.socket_name != "..",
            "Socket must be a basename");
        const char* runtime = getenv("XDG_RUNTIME_DIR");
        Require(runtime && *runtime, "XDG_RUNTIME_DIR is required");
        struct stat info{};
        Require(!lstat(runtime, &info) && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
            (info.st_mode & 0777) == 0700, "Runtime directory must belong to this user and have mode 0700");
        auto directory = std::filesystem::path(runtime) / "prism";
        if (mkdir(directory.c_str(), 0700) && errno != EEXIST) throw std::runtime_error("Cannot create launcher directory");
        Require(!lstat(directory.c_str(), &info) && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
            (info.st_mode & 0777) == 0700, "Launcher directory is not private");
        socket_path = directory / config.socket_name;
        const auto lock_path = socket_path.string() + ".lock";
        lock = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        Require(lock >= 0 && !fstat(lock, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
            (info.st_mode & 0777) == 0600 && !flock(lock, LOCK_EX | LOCK_NB), "Launcher endpoint is already locked or unsafe");
        sockaddr_un address{};
        Require(socket_path.string().size() < sizeof(address.sun_path), "Launcher socket path is too long");
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path.c_str(), socket_path.string().size() + 1);
        if (!lstat(socket_path.c_str(), &info)) {
            Require(S_ISSOCK(info.st_mode) && info.st_uid == geteuid(), "Refusing to replace a non-socket endpoint");
            int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            Require(probe >= 0, "Socket probe failed");
            const int result = connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            const int error = errno; close(probe);
            Require(result < 0 && error == ECONNREFUSED, "An existing endpoint is still live");
            Require(!unlink(socket_path.c_str()), "Cannot remove stale endpoint");
        } else Require(errno == ENOENT, "Cannot inspect launcher socket");
        listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        Require(listener >= 0, "Cannot create launcher socket");
        const auto mask = umask(0077);
        const int result = bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        umask(mask);
        Require(!result, "Cannot bind launcher socket");
        Require(!lstat(socket_path.c_str(), &info), "Cannot inspect bound socket");
        socket_device = info.st_dev; socket_inode = info.st_ino;
        Require(!chmod(socket_path.c_str(), 0600) && !listen(listener, 32), "Cannot listen on launcher socket");
        std::cout << "launcher socket=" << socket_path << " pool=" << config.pool_size << std::endl;
    }
    Endpoint* Find(Owner owner) {
        if (owner.worker) {
            auto it = workers.find(static_cast<pid_t>(owner.endpoint));
            return it == workers.end() ? nullptr : &it->second;
        }
        auto it = clients.find(owner.endpoint);
        return it == clients.end() ? nullptr : &it->second;
    }
    void Deliver(Owner owner, LaunchEvent event) {
        auto* endpoint = Find(owner);
        if (!endpoint || endpoint->stream->Closed()) return;
        event.request = {owner.request};
        auto frame = owner.worker ? launch::EncodeWorker(launch::WorkerReply{event}) : launch::EncodeMessage(event);
        endpoint->stream->Queue(frame);
    }
    bool Event(Job& job, LaunchMilestone milestone, LaunchError error = LaunchError::None,
               std::string detail = {}, int exit_code = 0) {
        LaunchEvent event{job.request.request, job.instance, job.state.Pid(), milestone, error, exit_code, std::move(detail)};
        if (!job.state.Apply(event)) return false;
        job.history.push_back(event); Deliver(job.owner, event); return true;
    }
    void DeliverUpdate(Owner owner, InstanceUpdate update) {
        auto* endpoint=Find(owner); if (!endpoint || endpoint->stream->Closed()) return;
        update.request={endpoint->subscription};
        endpoint->stream->Queue(owner.worker ? launch::EncodeWorker(update) : launch::EncodeMessage(update));
    }
    void Subscribe(Owner owner, InstanceSubscribe request) {
        auto* endpoint=Find(owner); if (!endpoint) return;
        if (endpoint->subscription || endpoint->requests.contains(request.request.value) || endpoint->theme_ids.contains(request.request.value)) { endpoint->stream->Close(); return; }
        endpoint->subscription=request.request.value;
        DeliverUpdate(owner,{{},{},0,InstanceChange::Reset,{}});
        for (const auto& [id,job]:jobs) if (!job->alias && job->role==WindowRole::Toplevel && job->mapped && !job->state.Terminal())
            DeliverUpdate(owner,{{},job->instance,job->state.Pid(),InstanceChange::Running,job->request.app_id});
        DeliverUpdate(owner,{{},{},0,InstanceChange::SnapshotDone,{}});
    }
    void WindowChanged(Job& job, bool mapped) {
        if (job.mapped==mapped) return;
        job.mapped=mapped;
        if (job.alias || job.role!=WindowRole::Toplevel) return;
        InstanceUpdate update{{},job.instance,job.state.Pid(),mapped ? InstanceChange::Running : InstanceChange::Stopped,job.request.app_id};
        for (const auto& [id,endpoint]:clients) if (endpoint.subscription) DeliverUpdate({false,id,0},update);
        for (const auto& [pid,worker]:workers) if (worker.subscription) DeliverUpdate({true,static_cast<unsigned>(pid),0},update);
    }
    void StopWorker(Worker& worker) {
        if (worker.phase != Worker::Phase::Stopping) {
            worker.phase = Worker::Phase::Stopping;
            worker.finish_at=0;
            worker.kill_at = Now() + 1000000000ULL;
            kill(worker.pid, SIGTERM);
        }
    }
    void FinishWorker(Worker& worker) {
        // A validated terminal report means the host is already exiting. Give
        // normal cleanup/exit a bounded turn before sending enforcement signals.
        // Cancellation, watchdog and session shutdown still call StopWorker.
        if(worker.phase!=Worker::Phase::Stopping&&worker.phase!=Worker::Phase::Finishing){
            worker.phase=Worker::Phase::Finishing;
            worker.finish_at=host::After(Now(),250000000ULL);
        }
    }
    ThemeEvent ThemeResult(std::uint64_t request,ThemeStatus status,std::string detail={}) const {
        return {request,committed_theme.generation,status,committed_theme.id,committed_theme.name,std::move(detail),committed_theme.color_scheme};
    }
    void DeliverTheme(Owner owner,const ThemeEvent& event) {
        if (auto* endpoint=Find(owner)) {
            if (event.request) endpoint->theme_requests[event.request]=event;
            endpoint->stream->Queue(owner.worker ? launch::EncodeWorker(event) : launch::EncodeMessage(event));
        }
    }
    void SendWmTheme() {
        if (!control || !session) return;
        launch::ControlMessage m; m.type=launch::ControlType::InstallTheme;m.permit.session=session;m.theme=theme;
        if (!control->Queue(launch::EncodeControl(m))) control_failed=true;
    }
    void SendWorkerTheme(Worker& worker) {
        if (worker.phase==Worker::Phase::Stopping || worker.phase==Worker::Phase::Finishing || !worker.frontend_ready || worker.stream->Closed()) return;
        if (theme_transaction) theme_transaction->pending.insert(worker.pid);
        if (!worker.stream->Queue(launch::EncodeWorker(theme))) StopWorker(worker);
    }
    void PublishThemeToHosts() {
        auto event=ThemeResult(0,ThemeStatus::Current);
        for (auto& [pid,worker]:workers) if (worker.phase==Worker::Phase::Assigned && !worker.stream->Closed())
            worker.stream->Queue(launch::EncodeWorker(event));
    }
    void FinishTheme() {
        if (!theme_transaction || theme_transaction->wm_pending || !theme_transaction->pending.empty()) return;
        auto t=std::move(*theme_transaction);theme_transaction.reset();
        committed_theme=theme;
        DeliverTheme(t.owner,ThemeResult(t.request,t.rollback?ThemeStatus::Rejected:ThemeStatus::Applied,t.detail));
        PublishThemeToHosts();
        std::cout<<"theme applied id="<<theme.id<<" scheme="<<theme.color_scheme<<" generation="<<theme.generation<<" rollback="<<t.rollback<<std::endl;
    }
    void RollbackTheme(std::string detail) {
        if (!theme_transaction) {control_failed=true;return;}
        auto& t=*theme_transaction;
        if (t.rollback) {std::cerr<<"Theme rollback failed: "<<detail<<std::endl;control_failed=true;return;}
        t.rollback=true;t.detail=std::move(detail);t.deadline=Now()+5000000000ULL;t.pending.clear();t.wm_pending=control!=nullptr;
        const auto next=theme.generation+1;Require(next,"Theme generation exhausted");theme=t.previous;theme.generation=next;
        SendWmTheme();for(auto& [pid,worker]:workers)SendWorkerTheme(worker);
        FinishTheme();
    }
    void ThemeAck(Worker& worker,const ThemeApplied& ack) {
        Require(worker.frontend_ready && ack.generation<=theme.generation,"Unexpected worker theme ACK");
        if (ack.generation!=theme.generation) return; // An older in-flight candidate can finish during rollback.
        if (!ack.success) {
            if (theme_transaction) RollbackTheme("Host rejected theme: "+ack.detail);
            else throw std::runtime_error("Host rejected initial theme: "+ack.detail);
            return;
        }
        worker.theme_generation=ack.generation;
        if (worker.phase==Worker::Phase::Preparing) worker.phase=Worker::Phase::Idle;
        if (theme_transaction) {theme_transaction->pending.erase(worker.pid);FinishTheme();}
    }
    void SelectTheme(Owner owner,const ThemeRequest& request) {
        auto* endpoint=Find(owner);if (!endpoint)return;
        Require(!endpoint->requests.contains(request.request) && endpoint->subscription!=request.request,"Theme request ID reused");
        if(auto identity=endpoint->theme_ids.find(request.request);identity!=endpoint->theme_ids.end()) {
            Require(identity->second==std::pair{request.id,request.color_scheme},"Theme replay ID refers to another appearance request");
            if(auto prior=endpoint->theme_requests.find(request.request);prior!=endpoint->theme_requests.end())DeliverTheme(owner,prior->second);
            return;
        }
        if(endpoint->theme_requests.size()>=256) {
            endpoint->theme_ids.erase(endpoint->theme_requests.begin()->first);
            endpoint->theme_requests.erase(endpoint->theme_requests.begin());
        }
        endpoint->theme_ids.emplace(request.request,std::pair{request.id,request.color_scheme});
        if(request.id.empty() && request.color_scheme.empty()){DeliverTheme(owner,ThemeResult(request.request,ThemeStatus::Current));return;}
        if(shutting_down||theme_transaction||!theme_ready){DeliverTheme(owner,ThemeResult(request.request,ThemeStatus::Rejected,"Theme service is busy"));return;}
        ThemeSnapshot candidate;
        try {candidate=prism::theme::LoadTheme(config.themes_root,request.id.empty()?committed_theme.id:request.id,theme.generation+1,
            request.color_scheme.empty()?committed_theme.color_scheme:request.color_scheme);}
        catch(const std::exception& e){DeliverTheme(owner,ThemeResult(request.request,ThemeStatus::Rejected,e.what()));return;}
        if(candidate.layout!=committed_theme.layout){DeliverTheme(owner,ThemeResult(request.request,ThemeStatus::Rejected,"Live Shell/BSP geometry changes require a configure-aware theme transaction"));return;}
        if (candidate.id==committed_theme.id && candidate.color_scheme==committed_theme.color_scheme && candidate.numbers==committed_theme.numbers && candidate.colors==committed_theme.colors && candidate.materials==committed_theme.materials && candidate.layout==committed_theme.layout && candidate.normal==committed_theme.normal && candidate.focused==committed_theme.focused && candidate.fullscreen==committed_theme.fullscreen && candidate.controls==committed_theme.controls) {
            DeliverTheme(owner,ThemeResult(request.request,ThemeStatus::Applied));return;
        }
        ThemeTransaction t;t.owner=owner;t.request=request.request;t.previous=committed_theme;t.deadline=Now()+5000000000ULL;t.wm_pending=control!=nullptr;
        theme_transaction=std::move(t);theme=std::move(candidate);
        SendWmTheme();for(auto& [pid,worker]:workers)SendWorkerTheme(worker);FinishTheme();
    }
    void SendControl(launch::ControlType type, const Job& job, std::uint64_t transaction=0) {
        if (!control || !session) return;
        launch::ControlMessage message; message.type=type;
        auto& p=message.permit; p.session=session; p.request={transaction ? transaction : job.request.request.value};
        p.instance=job.instance; p.pid=job.state.Pid(); p.role=job.role;
        if (type==launch::ControlType::Grant) {
            launch::RandomBytes(p.token); p.expires_ns=Now()+5000000000ULL;
        }
        if (!control->Queue(launch::EncodeControl(message))) control_failed=true;
    }
    void Fail(Job& job, LaunchError error, std::string detail) {
        if (!job.state.Terminal()) Event(job, LaunchMilestone::Failed, error, std::move(detail));
        if (job.alias) return; // An activation request never owns the existing worker.
        if (job.state.Pid()) SendControl(launch::ControlType::Revoke,job);
        if (auto pid = job.state.Pid()) {
            auto worker = workers.find(pid);
            if (worker != workers.end()) StopWorker(worker->second);
        }
        if (job.role!=WindowRole::Toplevel && !shutting_down) control_failed=true;
    }
    void BootstrapShell() {
        unsigned role=0;
        for (const char* app:{"prism_desktop","prism_topbar","prism_dock"}) {
            launch::LoadRegisteredPackage(config.apps_root,app);
            auto id=next_job++;
            auto job=std::make_unique<Job>(id,LaunchRequest{{id},app,LaunchMode::NewInstance},Owner{});
            job->role=static_cast<WindowRole>(++role); Event(*job,LaunchMilestone::Accepted);
            jobs.emplace(id,std::move(job));
        }
    }
    void ReadControl() {
        if (!control) return;
        try {
            for (const auto& frame:control->Receive()) {
                const auto m=launch::DecodeControl(frame); const auto& p=m.permit;
                if (m.type==launch::ControlType::Ready) {
                    Require(!session && m.success,"Unexpected WM ready"); session=p.session;
                    SendWmTheme();
                    continue;
                }
                Require(session && p.session==session,"WM session mismatch");
                if (m.type==launch::ControlType::ThemeApplied) {
                    Require(m.theme_applied.generation<=theme.generation,"Unexpected WM theme ACK");
                    if(m.theme_applied.generation!=theme.generation)continue;
                    if(!m.theme_applied.success){RollbackTheme("WM rejected theme: "+m.theme_applied.detail);continue;}
                    if(!theme_ready){theme_ready=true;if(config.start_shell)BootstrapShell();}
                    if(theme_transaction){theme_transaction->wm_pending=false;FinishTheme();}
                    continue;
                }
                if (m.type==launch::ControlType::Registered || m.type==launch::ControlType::Activated) {
                    auto it=jobs.find(p.request.value); Require(it!=jobs.end(),"Unknown WM transaction");
                    auto& job=*it->second;
                    Require(p.instance==job.instance && p.pid==job.state.Pid() && p.role==job.role,"WM identity mismatch");
                    if (job.state.Terminal()) continue;
                    if (!m.success) { Fail(job,LaunchError::RuntimeFailed,"WM rejected registration or activation"); continue; }
                    if (m.type==launch::ControlType::Registered) {
                        Require(!job.alias && !job.registered,"Duplicate registration ACK"); job.registered=true;
                        // Binding is gated in Maintain by the current acknowledged theme.
                    } else {
                        Require(job.alias && job.activation_sent,"Unexpected activation ACK");
                        Event(job,LaunchMilestone::Activated);
                    }
                } else if (m.type==launch::ControlType::Mapped || m.type==launch::ControlType::Unmapped) {
                    Job* found=nullptr;
                    for (auto& [id,job]:jobs) if (!job->alias && job->instance==p.instance && job->state.Pid()==p.pid) { found=job.get(); break; }
                    Require(found && found->role==p.role,"Unknown WM window");
                    WindowChanged(*found,m.type==launch::ControlType::Mapped);
                } else throw std::runtime_error("Unexpected WM reply");
            }
            if (control->Closed()) control_failed=true;
        } catch (const std::exception& error) {
            std::cerr << "WM control failed: " << error.what() << std::endl; control_failed=true;
        }
    }
    void Request(Owner owner, LaunchRequest source) {
        auto* endpoint = Find(owner);
        if (!endpoint) return;
        owner.request = source.request.value;
        if (owner.request==endpoint->subscription || endpoint->theme_ids.contains(owner.request) || (theme_transaction && theme_transaction->request==owner.request && theme_transaction->owner.worker==owner.worker && theme_transaction->owner.endpoint==owner.endpoint)) { endpoint->stream->Close(); return; }
        if (shutting_down) {
            Deliver(owner, {source.request, {}, 0, LaunchMilestone::Failed, LaunchError::SessionEnded, 0, "Launcher session is stopping"}); return;
        }
        if (auto prior = endpoint->requests.find(owner.request); prior != endpoint->requests.end()) {
            auto found = jobs.find(prior->second);
            if (found == jobs.end() || found->second->request.app_id != source.app_id ||
                found->second->request.mode != source.mode) { endpoint->stream->Close(); return; }
            for (auto event : found->second->history) Deliver(owner, event);
            return;
        }
        if (endpoint->requests.size() >= 256 || jobs.size() >= 4096) { endpoint->stream->Close(); return; }
        unsigned waiting = 0;
        for (const auto& [id, job] : jobs) if (!job->alias && !job->state.Terminal() && !job->state.Pid()) ++waiting;
        if (waiting >= 64) {
            Deliver(owner, {source.request, {}, 0, LaunchMilestone::Failed, LaunchError::NoWorker, 0, "Pending launch queue is full"}); return;
        }
        std::uint64_t activation_target=0;
        try {
            if (source.app_id=="prism_desktop" || source.app_id=="prism_topbar" || source.app_id=="prism_dock") throw launch::LaunchFailure(LaunchError::InvalidRequest,"Shell packages are private session applications");
            launch::LoadRegisteredPackage(config.apps_root, source.app_id);
            if (source.mode == LaunchMode::ActivateOrCreate) {
                for (const auto& [id, job] : jobs) if (!job->alias && job->role==WindowRole::Toplevel && job->request.app_id == source.app_id && !job->state.Terminal()) {
                    if (!control) throw launch::LaunchFailure(LaunchError::InvalidRequest,
                        "Existing instance activation requires a trusted WM control channel; use NewInstance");
                    activation_target=id; break;
                }
            }
        } catch (const launch::LaunchFailure& error) {
            Deliver(owner, {source.request, {}, 0, LaunchMilestone::Failed, error.Code(), 0, error.what()}); return;
        } catch (const std::exception&) {
            Deliver(owner, {source.request, {}, 0, LaunchMilestone::Failed, LaunchError::InvalidPackage, 0, "Cannot resolve application package"}); return;
        }
        auto id = next_job++;
        Require(id != 0, "Instance ID space exhausted");
        source.request = {id};
        auto job = std::make_unique<Job>(activation_target ? jobs.at(activation_target)->instance.value : id, std::move(source), owner);
        job->alias=activation_target;
        endpoint->requests[owner.request] = id;
        Event(*job, LaunchMilestone::Accepted);
        jobs.emplace(id, std::move(job));
    }
    void Cancel(Owner owner, LaunchCancel cancel) {
        auto* endpoint = Find(owner);
        if (!endpoint) return;
        owner.request = cancel.request.value;
        const auto found = endpoint->requests.find(owner.request);
        if (found == endpoint->requests.end()) {
            Deliver(owner, {cancel.request, {}, 0, LaunchMilestone::Failed, LaunchError::InvalidRequest, 0, "Unknown cancellation target"}); return;
        }
        auto& job = *jobs.at(found->second);
        if (job.state.Activated()) { for (auto event:job.history) Deliver(owner,event); return; }
        if (!job.state.Terminal()) Fail(job, LaunchError::Cancelled, "Cancelled by request owner");
        else for (auto event : job.history) Deliver(owner, event);
    }
    void Accept() {
        for (unsigned count = 0; count < 16; ++count) {
            const int fd = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            ucred credential{}; socklen_t length = sizeof(credential);
            if (clients.size() >= 64 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &length) ||
                credential.uid != geteuid()) { close(fd); continue; }
            Endpoint endpoint;
            endpoint.stream = std::make_unique<launch::Stream>(fd, launch::FrameSize);
            clients.emplace(next_endpoint++, std::move(endpoint));
        }
    }
    void ReadClient(std::uint64_t id, Endpoint& endpoint) {
        try {
            for (auto& frame : endpoint.stream->Receive()) {
                const auto message = launch::DecodeMessage(frame);
                if (const auto* request = std::get_if<LaunchRequest>(&message)) Request({false, id, 0}, *request);
                else if (const auto* cancel = std::get_if<LaunchCancel>(&message)) Cancel({false, id, 0}, *cancel);
                else if (const auto* subscribe=std::get_if<InstanceSubscribe>(&message)) Subscribe({false,id,0},*subscribe);
                else if (const auto* theme=std::get_if<ThemeRequest>(&message)) SelectTheme({false,id,0},*theme);
                else endpoint.stream->Close();
                if (endpoint.stream->Closed()) break;
            }
            if (endpoint.stream->HasPartialFrame()&&!endpoint.stream->HasCompleteFrame()) {
                if (!endpoint.partial_since) endpoint.partial_since = Now();
                if (Now() - endpoint.partial_since > 5000000000ULL) endpoint.stream->Close();
            } else endpoint.partial_since = 0;
        } catch (...) { endpoint.stream->Close(); }
    }
    void ReadWorker(Worker& worker) {
        try {
            for (auto& frame : worker.stream->Receive()) {
                const auto message = launch::DecodeWorker(frame);
                if (const auto* ready = std::get_if<launch::WorkerReady>(&message)) {
                    Require(worker.phase == Worker::Phase::Preparing, "Unexpected worker Ready");
                    worker.frontend_ready=true;SendWorkerTheme(worker);
                    std::cout << "worker ready pid=" << worker.pid << " preparation_ns=" << ready->preparation_ns << std::endl;
                } else if (const auto* ack=std::get_if<ThemeApplied>(&message)) {
                    ThemeAck(worker,*ack);
                } else if (const auto* request=std::get_if<ThemeRequest>(&message)) {
                    Require(worker.phase==Worker::Phase::Assigned,"Theme request from idle worker");
                    SelectTheme({true,static_cast<unsigned>(worker.pid),0},*request);
                } else if (const auto* event = std::get_if<LaunchEvent>(&message)) {
                    Require(worker.job != 0, "Event from idle worker");
                    Require(!control || (jobs.at(worker.job)->registered && jobs.at(worker.job)->bound_sent),"Worker event before bind/WM registration ACK");
                    auto& job = *jobs.at(worker.job);
                    Require(event->pid == static_cast<unsigned>(worker.pid) && event->request == job.request.request &&
                        event->instance == job.instance && event->milestone != LaunchMilestone::Accepted &&
                        event->milestone != LaunchMilestone::WorkerAssigned && event->milestone != LaunchMilestone::Exited &&
                        event->milestone != LaunchMilestone::Activated,
                        "Worker launch identity/milestone mismatch");
                    if (!job.state.Terminal()) {
                        Require(job.state.Apply(*event), "Out-of-order worker event");
                        job.history.push_back(*event); Deliver(job.owner, *event);
                        std::cout << "instance=" << job.instance.value << " app=" << job.request.app_id
                            << " pid=" << event->pid << " milestone=" << static_cast<unsigned>(event->milestone)
                            << " error=" << static_cast<unsigned>(event->error) << " detail=" << event->detail << std::endl;
                        if (!control && event->milestone==LaunchMilestone::FirstPresented) WindowChanged(job,true);
                        if (event->milestone==LaunchMilestone::Failed) {
                            SendControl(launch::ControlType::Revoke,job);
                            FinishWorker(worker); // Blocked cleanup still reaches TERM then KILL.
                            if (job.role!=WindowRole::Toplevel) control_failed=true;
                        }
                    }
                } else if (const auto* request = std::get_if<LaunchRequest>(&message)) {
                    Require(worker.phase == Worker::Phase::Assigned && worker.job != 0 && !jobs.at(worker.job)->state.Terminal(), "Launch from inactive worker");
                    Request({true, static_cast<unsigned>(worker.pid), 0}, *request);
                } else if (const auto* subscribe=std::get_if<InstanceSubscribe>(&message)) {
                    Require(worker.phase==Worker::Phase::Assigned,"Subscribe from idle worker");
                    Subscribe({true,static_cast<unsigned>(worker.pid),0},*subscribe);
                } else if (const auto* cancel = std::get_if<LaunchCancel>(&message)) {
                    Require(worker.phase == Worker::Phase::Assigned, "Cancel from idle worker");
                    Cancel({true, static_cast<unsigned>(worker.pid), 0}, *cancel);
                } else throw std::runtime_error("Unexpected worker message");
            }
        } catch (...) {
            worker.stream->Close();
            if (worker.job) Fail(*jobs.at(worker.job), LaunchError::RuntimeFailed, "Worker control protocol failed");
            StopWorker(worker);
        }
        if (worker.stream->Closed() && !worker.closed_at) worker.closed_at = Now();
    }
    bool Spawn() {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair)) return false;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
        if (pair[0] != 3) posix_spawn_file_actions_addclose(&actions, pair[0]);
        if (pair[1] != 3) posix_spawn_file_actions_addclose(&actions, pair[1]);
        // All service descriptors are CLOEXEC; only the private control endpoint survives.
        const auto path = config.host.string(), root = config.apps_root.string(), parent = std::to_string(getpid());
        const char* words[]{path.c_str(), "--worker-fd", "3", "--apps-root", root.c_str(),
            "--parent-pid", parent.c_str(), "--wayland", config.wayland.c_str(), nullptr};
        pid_t pid = 0;
        const int result = posix_spawn(&pid, path.c_str(), &actions, nullptr,
            const_cast<char* const*>(words), environ);
        posix_spawn_file_actions_destroy(&actions); close(pair[1]);
        if (result) { close(pair[0]); std::cerr << "worker spawn failed: " << std::strerror(result) << '\n'; return false; }
        Worker worker; worker.pid = pid; worker.created = Now();
        worker.stream = std::make_unique<launch::Stream>(pair[0], launch::WorkerFrameSize);
        workers.emplace(pid, std::move(worker));
        std::cout << "worker spawned pid=" << pid << std::endl;
        return true;
    }
    void Reap() {
        for (auto it = workers.begin(); it != workers.end();) {
            auto& worker = it->second;
            int status = 0;
            const auto reaped = waitpid(worker.pid, &status, WNOHANG);
            if (reaped < 0 && errno == ECHILD) {
                // This PID may already have been reused; never signal it now.
                if (worker.job) Event(*jobs.at(worker.job), LaunchMilestone::Failed,
                    LaunchError::RuntimeFailed, "Worker was reaped outside the service");
                workers.erase(it);
                throw std::runtime_error("Worker reaping ownership was lost");
            }
            if (reaped <= 0) { ++it; continue; }
            if (worker.job) {
                auto& job = *jobs.at(worker.job);
                const int exit_code = WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
                if (!job.state.Terminal() && (exit_code != 0 || !job.state.FirstPresented() || !job.state.BackendReady()))
                    Event(job, LaunchMilestone::Failed, LaunchError::RuntimeFailed, "Worker exited before successful completion or crashed");
                SendControl(launch::ControlType::Revoke,job);
                WindowChanged(job,false);
                Event(job, LaunchMilestone::Exited, LaunchError::None, {}, exit_code);
                for (auto& [id,alias]:jobs) if (alias->alias==worker.job) {
                    if (!alias->state.Pid()) Event(*alias,LaunchMilestone::Failed,LaunchError::RuntimeFailed,"Activation target exited before assignment");
                    else {
                        if (!alias->state.Terminal() && !alias->state.Activated()) Event(*alias,LaunchMilestone::Failed,LaunchError::RuntimeFailed,"Activation target exited");
                        Event(*alias,LaunchMilestone::Exited,LaunchError::None,{},exit_code);
                    }
                }
                if (job.role!=WindowRole::Toplevel && !shutting_down) control_failed=true;
            } else if (worker.phase == Worker::Phase::Preparing) retry_at = Now() + 1000000000ULL;
            std::cout << "worker reaped pid=" << worker.pid << std::endl;
            if(theme_transaction)theme_transaction->pending.erase(worker.pid);
            it = workers.erase(it);
            FinishTheme();
        }
    }
    void Maintain() {
        const auto now = Now();
        if(theme_transaction && now>=theme_transaction->deadline)RollbackTheme("Theme installation timed out");
        for (auto& [id, job] : jobs) {
            if (!job->state.Terminal() && !job->state.Activated() && (!job->state.FirstPresented() || !job->state.BackendReady()) &&
                now - job->created >= config.startup_timeout_ms * 1000000ULL)
                Fail(*job, LaunchError::Timeout, "Worker allocation/presentation/backend watchdog expired");
        }
        for (auto& [pid, worker] : workers) {
            if(worker.phase==Worker::Phase::Finishing&&now>=worker.finish_at)StopWorker(worker);
            if (worker.phase == Worker::Phase::Preparing && now - worker.created >= config.startup_timeout_ms * 1000000ULL)
                StopWorker(worker);
            if (worker.closed_at && now - worker.closed_at >= 250000000ULL) {
                if (worker.job && !jobs.at(worker.job)->state.Terminal())
                    Fail(*jobs.at(worker.job), LaunchError::RuntimeFailed, "Worker control connection lost");
                StopWorker(worker);
            }
            if (worker.phase == Worker::Phase::Stopping && worker.kill_at && now >= worker.kill_at) {
                kill(pid, SIGKILL);worker.kill_at=0; // Consumed; SIGCHLD wakes reaping.
            }
        }
        if(!theme_transaction && theme_ready) for(auto& [id,job]:jobs) {
            if(job->alias || !job->registered || job->bound_sent || job->state.Terminal())continue;
            auto found=workers.find(job->state.Pid());if(found==workers.end())continue;
            auto& worker=found->second;
            if(worker.theme_generation!=theme.generation || worker.phase!=Worker::Phase::Assigned)continue;
            job->bound_sent=true;
            if(!worker.stream->Queue(launch::EncodeWorker(launch::WorkerBind{job->request,job->instance})))
                Fail(*job,LaunchError::RuntimeFailed,"Worker bind failed");
        }
        for (auto& [id,job]:jobs) if (job->alias && !job->state.Terminal() && !job->state.Activated()) {
            auto target=jobs.find(job->alias);
            if (target==jobs.end() || target->second->state.Terminal()) { Fail(*job,LaunchError::RuntimeFailed,"Activation target ended"); continue; }
            auto& original=*target->second;
            if (!job->state.Pid() && original.state.Pid()) {
                LaunchEvent assigned{job->request.request,job->instance,original.state.Pid(),LaunchMilestone::WorkerAssigned,LaunchError::None,0,{}};
                Require(job->state.Apply(assigned),"Activation assignment error"); job->history.push_back(assigned); Deliver(job->owner,assigned);
            }
            if (original.mapped && !job->activation_sent) {
                job->activation_sent=true; SendControl(launch::ControlType::Activate,*job);
            }
        }
        for (auto& [id, job] : jobs) if (!job->alias && !job->state.Terminal() && !job->state.Pid() && theme_ready && !theme_transaction && (!control || session)) {
            auto idle = std::find_if(workers.begin(), workers.end(), [&](const auto& item) {
                return item.second.phase == Worker::Phase::Idle && item.second.theme_generation==theme.generation && !item.second.stream->Closed();
            });
            if (idle == workers.end()) break;
            auto& worker = idle->second;
            worker.phase = Worker::Phase::Assigned; worker.job = id;
            LaunchEvent assigned{job->request.request, job->instance, static_cast<unsigned>(worker.pid), LaunchMilestone::WorkerAssigned, LaunchError::None, 0, {}};
            Require(job->state.Apply(assigned), "Internal worker assignment state error");
            job->history.push_back(assigned); Deliver(job->owner, assigned);
            if (control) SendControl(launch::ControlType::Grant,*job);
            else {
                job->bound_sent=true;
                if (!worker.stream->Queue(launch::EncodeWorker(launch::WorkerBind{job->request, job->instance})))
                    Fail(*job, LaunchError::RuntimeFailed, "Worker assignment send failed");
            }
        }
        unsigned waiting = 0, unbound = 0;
        for (const auto& [id, job] : jobs) if (!job->alias && !job->state.Terminal() && !job->state.Pid()) ++waiting;
        for (const auto& [pid, worker] : workers)
            if (worker.phase == Worker::Phase::Idle || worker.phase == Worker::Phase::Preparing) ++unbound;
        const auto wanted = std::max(config.pool_size, waiting);
        for (auto it = workers.rbegin(); it != workers.rend() && unbound > wanted; ++it) {
            auto& worker = it->second;
            if (worker.phase == Worker::Phase::Idle || worker.phase == Worker::Phase::Preparing) {
                StopWorker(worker); --unbound;
            }
        }
        if (!shutting_down && now >= retry_at && unbound < wanted && workers.size() < config.max_workers) {
            if (!Spawn()) retry_at = now + 1000000000ULL;
        }
        for (auto it = clients.begin(); it != clients.end();)
            if (it->second.stream->Closed()) it = clients.erase(it); else ++it;
        for (auto it = jobs.begin(); it != jobs.end();)
            if (it->second->state.Terminal() && (!it->second->state.Pid() || it->second->state.ExitCode()) &&
                !Find(it->second->owner)) it = jobs.erase(it); else ++it;
    }
    void Flush() {
        if (control) control->Flush();
        for (auto& [id, endpoint] : clients) endpoint.stream->Flush();
        for (auto& [pid, worker] : workers) worker.stream->Flush();
    }
    int WaitTimeout() const {
        const auto now=Now();std::optional<std::uint64_t> deadline;
        if(control&&!theme_ready)host::Earlier(deadline,host::After(opened_at,10000000000ULL));
        if(theme_transaction)host::Earlier(deadline,theme_transaction->deadline);
        for(const auto& [id,endpoint]:clients){
            if(endpoint.stream->HasCompleteFrame())return 0;
            if(endpoint.partial_since)host::Earlier(deadline,host::After(endpoint.partial_since,5000000000ULL));
        }
        if(control&&control->HasCompleteFrame())return 0;
        for(const auto& [id,job]:jobs)
            if(!job->state.Terminal()&&!job->state.Activated()&&(!job->state.FirstPresented()||!job->state.BackendReady()))
                host::Earlier(deadline,host::After(job->created,config.startup_timeout_ms*1000000ULL));
        unsigned waiting{},unbound{};
        for(const auto& [id,job]:jobs)if(!job->alias&&!job->state.Terminal()&&!job->state.Pid())++waiting;
        for(const auto& [pid,worker]:workers){
            if(worker.stream->HasCompleteFrame())return 0;
            if(worker.phase==Worker::Phase::Idle||worker.phase==Worker::Phase::Preparing)++unbound;
            if(worker.phase==Worker::Phase::Preparing)
                host::Earlier(deadline,host::After(worker.created,config.startup_timeout_ms*1000000ULL));
            if(worker.phase==Worker::Phase::Finishing)host::Earlier(deadline,worker.finish_at);
            if(worker.closed_at&&worker.phase!=Worker::Phase::Stopping)
                host::Earlier(deadline,host::After(worker.closed_at,250000000ULL));
            if(worker.phase==Worker::Phase::Stopping&&worker.kill_at)host::Earlier(deadline,worker.kill_at);
        }
        if(unbound<std::max(config.pool_size,waiting)&&workers.size()<config.max_workers)
            host::Earlier(deadline,retry_at);
        // Only legacy direct callers without a signal source need a reaping
        // fallback. The production executable always supplies its self-pipe.
        return host::Timeout(now,deadline,signal_fd<0?1000:-1);
    }
    void Shutdown() {
        if (shutting_down) return;
        shutting_down = true;
        for (auto& [id, job] : jobs) Fail(*job, LaunchError::SessionEnded, "Launcher session ended");
        for (auto& [pid, worker] : workers) StopWorker(worker);
        const auto deadline = Now() + 2000000000ULL;
        while (!workers.empty() && Now() < deadline) {
            for (auto& [pid, worker] : workers) {
                ReadWorker(worker);
                if (worker.kill_at&&Now() >= worker.kill_at){kill(pid, SIGKILL);worker.kill_at=0;}
            }
            Reap();Flush();
            if(workers.empty())break;
            std::optional<std::uint64_t> next=deadline;
            std::vector<pollfd> wait;
            if(signal_fd>=0)wait.push_back({signal_fd,POLLIN,0});
            for(const auto& [pid,worker]:workers){
                if(worker.kill_at)host::Earlier(next,worker.kill_at);
                if(!worker.stream->Closed())wait.push_back({worker.stream->Fd(),static_cast<short>(POLLIN|(worker.stream->WantsWrite()?POLLOUT:0)),0});
            }
            const auto result=poll(wait.data(),wait.size(),host::Timeout(Now(),next,signal_fd<0?1000:-1));
            if(result<0&&errno!=EINTR)break;
            if(signal_fd>=0&&!wait.empty()&&wait.front().revents)host::Drain(signal_fd);
        }
        for (auto& [pid, worker] : workers) {
            kill(pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
        workers.clear(); Flush();
    }
    int Run(const volatile std::sig_atomic_t& stopping,int wake_fd) {
        signal_fd=wake_fd;
        Open();
        while (!stopping && !control_failed) {
            ReadControl();
            if (control && !theme_ready && Now()-opened_at>=10000000000ULL) control_failed=true;
            Accept();
            for (auto& [id, endpoint] : clients) ReadClient(id, endpoint);
            for (auto& [pid, worker] : workers) ReadWorker(worker);
            Reap(); Maintain(); Flush();
            if(stopping||control_failed)break;
            std::vector<pollfd> descriptors{{listener, POLLIN, 0}};
            if(signal_fd>=0)descriptors.push_back({signal_fd,POLLIN,0});
            if (control && !control->Closed()) descriptors.push_back({control->Fd(),static_cast<short>(POLLIN | (control->WantsWrite()?POLLOUT:0)),0});
            for (const auto& [id, endpoint] : clients) if (!endpoint.stream->Closed())
                descriptors.push_back({endpoint.stream->Fd(), static_cast<short>(POLLIN | (endpoint.stream->WantsWrite() ? POLLOUT : 0)), 0});
            for (const auto& [pid, worker] : workers) if (!worker.stream->Closed())
                descriptors.push_back({worker.stream->Fd(), static_cast<short>(POLLIN | (worker.stream->WantsWrite() ? POLLOUT : 0)), 0});
            const int result=poll(descriptors.data(),descriptors.size(),WaitTimeout());
            if(result<0&&errno!=EINTR)throw std::runtime_error("Launcher event wait failed");
            if(signal_fd>=0&&descriptors[1].revents)host::Drain(signal_fd);
        }
        Shutdown(); return control_failed ? 1 : 0;
    }
};
Service::Service(ServiceConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Service::~Service() = default;
int Service::Run(const volatile std::sig_atomic_t& stopping,int signal_fd) { return impl_->Run(stopping,signal_fd); }
} // namespace prism::launcher
