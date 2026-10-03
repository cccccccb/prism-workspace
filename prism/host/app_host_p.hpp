#pragma once
#include "prism/host/event_wait.hpp"
#include "prism/launch/error.hpp"
#include "prism/runtime/dsl_schema.hpp"
#include "prism/runtime/image_resources.hpp"
#include "prism/runtime/master_load_session.hpp"
#include "prism/sdk/app_host.hpp"
#include "prism/sdk/client_application.hpp"
#include "prism/sdk/launch_client.hpp"
#include "prism/sdk/module_session.hpp"
#include "prism/theme/compiler.hpp"
#include <cerrno>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <unistd.h>
#include <vector>

namespace prism::sdk {
namespace host_detail {
runtime::PreparedComponent PrepareUi(const std::filesystem::path &, std::string);
std::string UiFailureDetail(const runtime::LoadDiagnostic &);
bool CallerInputReady(std::span<const pollfd>);
void CollectBindings(const runtime::PreparedNode &, std::set<std::string, std::less<>> &);
bool MatchesBinding(runtime::LoadBindingType, const runtime::PropertyValue &);

class DurationTimer {
public:
    explicit DurationTimer(std::uint64_t &microseconds)
        : value_(microseconds), start_(MonotonicNs())
    {
    }

    ~DurationTimer()
    {
        value_ = (MonotonicNs() - start_) / 1000;
    }

private:
    std::uint64_t &value_;
    std::uint64_t start_;
};

class PumpProcessingTimer {
public:
    PumpProcessingTimer(HostStartupStats &, const ClientApplication &, const std::uint64_t &);
    ~PumpProcessingTimer();

private:
    HostStartupStats &stats_;
    const ClientApplication &frontend_;
    const std::uint64_t &host_wait_ns_;
    std::uint64_t start_ns_, wait_ns_;
};
} // namespace host_detail

struct AppHost::Impl {
    explicit Impl(HostConfig value) : config(std::move(value))
    {
    }

    HostConfig config;
    std::unique_ptr<ClientApplication> frontend;
    std::unique_ptr<ModuleSession> business;
    std::unique_ptr<LaunchClient> launches;
    std::optional<launch::AppPackage> package;
    std::shared_ptr<runtime::TaskScheduler> scheduler;
    std::unique_ptr<runtime::MasterLoadSession> master_loader;
    std::optional<runtime::MasterLoadCompletion> master_completion;
    bool master_install_started{}, install_advanced{};
    bool business_work_dispatched{}, business_progress{};
    std::shared_ptr<const runtime::LoadPlan> installed_plan;
    runtime::BindingValues bindings;
    std::set<std::string, std::less<>> mounted_bindings;
    std::map<std::string, runtime::PreparedComponent, std::less<>> pending_regions;
    std::set<std::string, std::less<>> installed_regions, rejected_regions;
    std::vector<runtime::PreparedRegion> installing_regions;
    std::vector<runtime::LoadDiagnostic> region_diagnostics;
    std::size_t region_batch_limit{4};
    HostUiState ui;
    HostStartupStats startup;
    std::uint64_t wait_duration_ns{};
    std::uint64_t startup_deadline{};
    bool deferred_presentation{};
    bool configured{}, presented{}, ready{}, failed{}, bound_once{}, launch_disconnected{};
    bool window_open{}, closed{};

    void Event(contracts::LaunchMilestone milestone,
               contracts::LaunchError error = contracts::LaunchError::None,
               std::string detail = {});
    bool Fail(contracts::LaunchError error, std::string detail);
    void Observe();
    void ObserveStartup();
    bool SetBinding(std::string_view key, runtime::PropertyValue value);
    std::uint64_t LaunchApplication(std::string_view app_id);
    std::uint64_t SubscribeInstances();
    std::uint64_t SelectTheme(std::string_view id);
    std::uint64_t SelectColorScheme(std::string_view scheme);
    void HandleAction(std::string_view action);
    void HandleGesture(const contracts::GestureEvent &event);
    bool StartBusiness();
    void DispatchBusinessWork();
    bool StartMasterPreparation();
    void UiSubmitted(runtime::UiLoadId load);
    void TakeMasterCompletion();
    bool InstallMaster();
    void TakeRegionCompletions();
    bool InstallRegions();
    bool RegionsNeedWork() const;
    void RejectRegion(std::string_view, runtime::LoadDiagnostic);
    void RejectDependentRegions();
    void DrainLaunches();
    AppHost *owner{};
};
} // namespace prism::sdk
