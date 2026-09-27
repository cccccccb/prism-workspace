#include "prism/sdk/app_host.hpp"
#include "prism/sdk/client_application.hpp"
#include "prism/sdk/module_session.hpp"
#include "prism/sdk/launch_client.hpp"
#include "prism/launch/error.hpp"
#include "prism/theme/compiler.hpp"
#include "prism/host/event_wait.hpp"
#include <fstream>
#include <iterator>
#include <vector>
#include <unistd.h>

namespace prism::sdk {
namespace {
std::string ReadUi(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw launch::LaunchFailure(contracts::LaunchError::InvalidPackage, "Cannot read package UI");
    std::string source;
    char bytes[4096];
    while (input.read(bytes, sizeof(bytes)) || input.gcount()) {
        source.append(bytes, input.gcount());
        if (source.size() > 1024 * 1024)
            throw launch::LaunchFailure(contracts::LaunchError::InvalidPackage, "UI exceeds 1 MiB");
    }
    if (input.bad() || source.empty())
        throw launch::LaunchFailure(contracts::LaunchError::InvalidPackage, "Invalid package UI");
    return source;
}
}
struct AppHost::Impl {
    explicit Impl(HostConfig value) : config(std::move(value)) {}
    HostConfig config;
    std::unique_ptr<ClientApplication> frontend;
    std::unique_ptr<ModuleSession> business;
    std::unique_ptr<LaunchClient> launches;
    std::optional<launch::AppPackage> package;
    std::uint64_t startup_deadline{};
    bool configured{}, presented{}, ready{}, failed{}, bound_once{},launch_disconnected{};
    void Event(contracts::LaunchMilestone milestone, contracts::LaunchError error = contracts::LaunchError::None,
               std::string detail = {}) {
        if (config.on_event) config.on_event({config.request, config.instance,
            static_cast<std::uint32_t>(getpid()), milestone, error, 0, std::move(detail)});
    }
    bool Fail(contracts::LaunchError error, std::string detail) {
        if (!failed) { failed = true; Event(contracts::LaunchMilestone::Failed, error, std::move(detail)); }
        return false;
    }
    void Observe() {
        if (!configured && frontend->ConfigureCount()) {
            configured = true; Event(contracts::LaunchMilestone::SurfaceConfigured);
        }
        if (!presented && frontend->PresentationCount()) {
            presented = true;
            Event(contracts::LaunchMilestone::FirstPresented, contracts::LaunchError::None,
                "GL renderer=" + frontend->GlRenderer());
        }
        if (!ready && business && business->BackendReady()) {
            ready = true; Event(contracts::LaunchMilestone::BackendReady);
        }
    }
    bool StartBusiness() {
        business = std::make_unique<ModuleSession>(package->module, package->manifest.app_id,
            config.instance.value, [this](std::string_view key, runtime::PropertyValue value) {
                return frontend->SetBinding(key, std::move(value));
            }, [this](std::string_view app_id) {
                if (config.launch_app) return config.launch_app(app_id);
                if (!launches) launches = std::make_unique<LaunchClient>();
                return launches->Launch(std::string(app_id));
            }, [this]() {
                if (config.subscribe_instances) return config.subscribe_instances();
                if (!launches) launches=std::make_unique<LaunchClient>();
                return launches->SubscribeInstances();
            }, [this](std::string_view id) {
                if (config.select_theme) return config.select_theme(id);
                if (!launches) launches=std::make_unique<LaunchClient>();
                return launches->SelectTheme(std::string(id));
            }, [this](std::string_view scheme) {
                if (config.select_color_scheme) return config.select_color_scheme(scheme);
                if (!launches) launches=std::make_unique<LaunchClient>();
                return launches->SelectTheme({},std::string(scheme));
            });
        if (!business->Start()) return Fail(contracts::LaunchError::RuntimeFailed, "Business create failed");
        frontend->OnAction([this](std::string_view action) { business->Action(action); });
        if (config.initial_theme) {
            const auto& theme=*config.initial_theme;
            business->Deliver(contracts::ThemeEvent{0,theme.generation,contracts::ThemeStatus::Current,
                theme.id,theme.name,{},theme.color_scheme});
        }
        return true;
    }
    void DrainLaunches() {
        if(!launches||!business)return;
        for(const auto& event:launches->Pump(0))business->Deliver(event);
        for(const auto& event:launches->TakeInstanceUpdates())business->Deliver(event);
        for(const auto& event:launches->TakeThemeEvents()){
            if(event.status!=contracts::ThemeStatus::Rejected){
                std::string diagnostic;
                if(!owner->ApplyTheme(theme::LoadTheme(theme::DefaultThemeRoot(),event.id,event.generation,event.color_scheme),&diagnostic)){
                    business->Deliver(contracts::ThemeEvent{event.request,owner->ThemeGeneration(),
                        contracts::ThemeStatus::Rejected,event.id,event.name,std::move(diagnostic),event.color_scheme});
                    continue;
                }
            }
            business->Deliver(event);
        }
        if(launches->Connected())launch_disconnected=false;
        else if(!launch_disconnected){launch_disconnected=true;business->Disconnected();}
    }
    AppHost* owner{};
};
AppHost::AppHost(HostConfig config) : impl_(std::make_unique<Impl>(std::move(config))) { impl_->owner=this; }
AppHost::~AppHost() { Close(); }
bool AppHost::PrepareFrontend() {
    auto& self = *impl_;
    if (self.failed || self.bound_once) return false;
    if (self.frontend) return self.frontend->FrontendReady();
    try {
        ClientConfig config;
        config.font_path = self.config.font_path;
        self.frontend = std::make_unique<ClientApplication>(std::move(config));
        if (!self.frontend->FrontendReady()) return self.Fail(contracts::LaunchError::RuntimeFailed, "Font initialization failed");
        if (self.config.initial_theme) {
            std::string diagnostic;
            if (!self.frontend->ApplyTheme(*self.config.initial_theme,&diagnostic))
                return self.Fail(contracts::LaunchError::RuntimeFailed,std::move(diagnostic));
        }
        return true;
    } catch (const std::exception& error) { return self.Fail(contracts::LaunchError::RuntimeFailed, error.what()); }
}
bool AppHost::Assign(contracts::RequestId request, contracts::InstanceId instance) {
    if (impl_->bound_once || impl_->failed || !request.value || !instance.value) return false;
    impl_->config.request = request; impl_->config.instance = instance; return true;
}
void AppHost::DeliverLaunchEvent(const contracts::LaunchEvent& event) {
    if (impl_->business) impl_->business->Deliver(event);
}
void AppHost::DeliverInstanceEvent(const contracts::InstanceUpdate& event) {
    if (impl_->business) impl_->business->Deliver(event);
}
void AppHost::DeliverThemeEvent(const contracts::ThemeEvent& event) {
    if (impl_->business) impl_->business->Deliver(event);
}
bool AppHost::ApplyTheme(const contracts::ThemeSnapshot& theme, std::string* diagnostic) {
    try {
        contracts::ValidateTheme(theme);
        std::optional<contracts::ThemeSnapshot> prepared(theme);
        if (impl_->frontend && !impl_->frontend->ApplyTheme(theme,diagnostic)) return false;
        impl_->config.initial_theme.swap(prepared);
        if (diagnostic) diagnostic->clear();
        return true;
    } catch (const std::exception& error) {
        if (diagnostic) *diagnostic=error.what();
        return false;
    }
}
std::uint64_t AppHost::ThemeGeneration() const {
    return impl_->config.initial_theme ? impl_->config.initial_theme->generation : 0;
}
bool AppHost::Bind(const launch::AppPackage& package) {
    auto& self = *impl_;
    if (self.bound_once || self.failed || !self.config.request.value || !self.config.instance.value) return false;
    if (!PrepareFrontend()) return false;
    self.bound_once = true;
    self.package = package;
    self.startup_deadline = MonotonicNs() + 10000000000ULL;
    try {
        if (!self.config.initial_theme) {
            const auto initial=theme::LoadTheme(theme::DefaultThemeRoot(),"glass",1);
            std::string diagnostic;
            if (!ApplyTheme(initial,&diagnostic))
                return self.Fail(contracts::LaunchError::RuntimeFailed,std::move(diagnostic));
        }
        const auto& manifest = package.manifest;
        ClientConfig config{self.config.socket, manifest.app_id, manifest.name, self.config.font_path,
            manifest.width, manifest.height, package.assets.string(),self.config.gpu_resource_cache_bytes};
        if (!self.frontend->ConfigureWindow(std::move(config)) ||
            !self.frontend->Open(ReadUi(package.preview ? *package.preview : package.ui)))
            return self.Fail(contracts::LaunchError::RuntimeFailed, "Frontend open failed");
        if (!self.frontend->HasPresentationFeedback())
            return self.Fail(contracts::LaunchError::PresentationFailed, "Compositor lacks presentation-time");
        self.Event(contracts::LaunchMilestone::RuntimeReady);
        // Preview is presented before dlopen/create. The next Pump performs the
        // master transition in this same surface. No WM-side UI is involved.
        if (!package.preview && !self.StartBusiness()) return false;
        self.Observe();
        return true;
    } catch (const launch::LaunchFailure& error) { return self.Fail(error.Code(), error.what()); }
      catch (const std::exception& error) { return self.Fail(contracts::LaunchError::RuntimeFailed, error.what()); }
}
bool AppHost::Pump(int timeout,std::span<pollfd> wake_fds) {
    auto& self = *impl_;
    if (!self.package || self.failed) return false;
    try {
        self.DrainLaunches();
        if(self.business)self.business->Tick(MonotonicNs());
        const auto now=MonotonicNs();
        int wait=self.business?self.business->TimeoutMs(now,timeout):timeout;
        if(!self.presented||!self.ready)wait=host::Timeout(now,self.startup_deadline,wait);
        std::vector<pollfd> descriptors(wake_fds.begin(),wake_fds.end());
        for(auto& fd:descriptors)fd.revents=0;
        if(self.launches&&self.launches->Connected()){
            descriptors.push_back({self.launches->Fd(),static_cast<short>(POLLIN|(self.launches->WantsWrite()?POLLOUT:0)),0});
            if(self.launches->HasCompleteFrame())wait=0;
        }
        if (!self.frontend->Pump(wait,descriptors)) {
            if (self.frontend->IsCloseRequested()) return false;
            return self.Fail(contracts::LaunchError::RuntimeFailed, "Wayland/render connection failed");
        }
        for(std::size_t i=0;i<wake_fds.size();++i)wake_fds[i].revents=descriptors[i].revents;
        self.DrainLaunches();
        self.Observe();
        if (!self.business && self.presented) {
            if (!self.frontend->ReplaceUi(ReadUi(self.package->ui)))
                return self.Fail(contracts::LaunchError::RuntimeFailed, "Master UI replacement failed");
            if (!self.StartBusiness()) return false;
            self.Observe();
        }
        if(self.business)self.business->Tick(MonotonicNs());
        self.Observe();
        if ((!self.presented || !self.ready) && MonotonicNs() >= self.startup_deadline)
            return self.Fail(contracts::LaunchError::Timeout, "Presentation/backend startup timeout");
        return true;
    } catch (const launch::LaunchFailure& error) { return self.Fail(error.Code(), error.what()); }
      catch (const std::exception& error) { return self.Fail(contracts::LaunchError::RuntimeFailed, error.what()); }
}
bool AppHost::IsCloseRequested() const { return impl_->frontend && impl_->frontend->IsCloseRequested(); }
void AppHost::Close() {
    auto& self = *impl_;
    if (self.frontend) self.frontend->OnAction({});
    self.business.reset();
    self.launches.reset();
    self.frontend.reset();
    self.package.reset();
}
} // namespace prism::sdk
