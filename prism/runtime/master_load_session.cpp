#include "master_load_session_p.hpp"
#include <algorithm>
#include <stdexcept>

namespace prism::runtime {
MasterLoadSession::MasterLoadSession(std::shared_ptr<TaskScheduler> scheduler,
                                     PrepareFunction prepare)
    : impl_(std::make_unique<Impl>())
{
    impl_->scheduler = scheduler ? std::move(scheduler) : std::make_shared<TaskScheduler>();
    impl_->channel = impl_->scheduler->OpenChannel();
    impl_->prepare = std::move(prepare);
}

MasterLoadSession::~MasterLoadSession()
{
    Stop();
}

LoadSubmitResult MasterLoadSession::Submit(MasterLoadRequest request)
{
    auto &self = *impl_;
    if (self.stopped) {
        return LoadSubmitResult::Closed;
    }
    if (self.request) {
        return LoadSubmitResult::Busy;
    }
    if (!request.load.owner || request.entry.empty() || request.package_root.empty()) {
        return LoadSubmitResult::Invalid;
    }
    const ComponentSource source{"master", request.entry.string(), {}};
    const auto result = self.channel->Submit(
        {1, TaskPriority::Critical, kDslReservation,
         MasterReadWork{
             request.package_root, request.entry, source, {}, self.prepare, true, false}});
    if (result != TaskSubmitResult::Accepted) {
        return result == TaskSubmitResult::Busy ? LoadSubmitResult::Busy : LoadSubmitResult::Closed;
    }
    self.request = std::move(request);
    return LoadSubmitResult::Accepted;
}

bool MasterLoadSession::Impl::DependenciesReady(const LoadUnit &unit) const
{
    for (const auto &name : unit.after) {
        const auto found = std::find_if(plan->components.begin(), plan->components.end(),
                                        [&](const LoadUnit &other) { return other.id == name; });
        if (found == plan->components.end() || !units[found - plan->components.begin()].prepared) {
            return false;
        }
    }
    return true;
}

void MasterLoadSession::Impl::CancelTasks()
{
    for (std::size_t id = 1; id <= kMaxLoadComponents + 3; ++id) {
        channel->Cancel(id);
    }
}

void MasterLoadSession::Impl::Fail(LoadDiagnostic diagnostic)
{
    if (failed || delivered) {
        return;
    }
    failed = true;
    CancelTasks();
    completion = MasterLoadCompletion{request->load, plan, {}, std::move(diagnostic), timings};
}

void MasterLoadSession::Impl::Dispatch()
{
    if (!plan || plan->legacy || failed || stopped) {
        return;
    }
    if (stats.deferred_started) {
        bool changed;
        do {
            changed = false;
            for (std::size_t i = 0; i < units.size(); ++i) {
                if (units[i].complete || units[i].submitted ||
                    plan->components[i].phase != LoadPhase::Deferred) {
                    continue;
                }
                for (const auto &name : plan->components[i].after) {
                    const auto dependency =
                        std::find_if(plan->components.begin(), plan->components.end(),
                                     [&](const LoadUnit &unit) { return unit.id == name; });
                    const auto index = dependency - plan->components.begin();
                    if (dependency != plan->components.end() && units[index].complete &&
                        !units[index].prepared) {
                        units[i].complete = units[i].submitted = true;
                        const auto &unit = plan->components[i];
                        deferred_diagnostics.push_back(
                            {LoadStage::Semantic,
                             {unit.id, (plan->package_root / unit.source_path).string(), {}},
                             unit.line,
                             "Component dependency failed: " + name});
                        region_completions.push_back(
                            {request->load, unit.id, {}, deferred_diagnostics.back()});
                        changed = true;
                        break;
                    }
                }
            }
        } while (changed);
    }
    if (!layout_submitted) {
        const auto file = plan->package_root / plan->layout_path;
        const auto result = channel->Submit(
            {2, TaskPriority::Critical, kDslReservation,
             MasterReadWork{
                 plan->package_root, file, {"layout", file.string(), {}}, plan, {}, false, true}});
        if (result == TaskSubmitResult::Accepted) {
            layout_submitted = true;
        } else if (result != TaskSubmitResult::Busy) {
            Fail({LoadStage::Read, plan->source, 0, "Cannot schedule layout preparation"});
            return;
        }
    }
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto &unit = plan->components[i];
        if (units[i].submitted || (unit.phase == LoadPhase::Deferred && !stats.deferred_started) ||
            !DependenciesReady(unit)) {
            continue;
        }
        const auto file = plan->package_root / unit.source_path;
        const auto result = channel->Submit(
            {i + 4,
             unit.phase == LoadPhase::Critical ? TaskPriority::Critical : TaskPriority::Deferred,
             kDslReservation,
             MasterReadWork{plan->package_root,
                            file,
                            {unit.id, file.string(), {}},
                            plan,
                            prepare,
                            false,
                            false}});
        if (result == TaskSubmitResult::Accepted) {
            units[i].submitted = true;
        } else if (result == TaskSubmitResult::Busy) {
            break;
        } else {
            Fail({LoadStage::Read,
                  {unit.id, file.string(), {}},
                  unit.line,
                  "Cannot schedule component preparation"});
            return;
        }
    }

    if (!composing && layout && !stats.critical_ready) {
        std::vector<PreparedUnit> critical;
        for (std::size_t i = 0; i < units.size(); ++i) {
            if (plan->components[i].phase == LoadPhase::Critical) {
                if (!units[i].prepared) {
                    return;
                }
                critical.push_back({plan->components[i].id, *units[i].prepared});
            }
        }
        const auto result =
            channel->Submit({3, TaskPriority::Critical, kDslReservation,
                             MasterComposeWork{plan, *layout, std::move(critical)}});
        composing = result == TaskSubmitResult::Accepted;
        if (!composing && result != TaskSubmitResult::Busy) {
            Fail({LoadStage::Semantic, plan->source, 0, "Cannot schedule critical composition"});
        }
    }
}

void MasterLoadSession::Impl::Complete(TaskCompletion result)
{
    if (!request || stopped || failed) {
        return;
    }
    const auto output = std::dynamic_pointer_cast<const MasterTaskOutput>(result.output);
    const bool deferred = result.id >= 4 && result.id - 4 < units.size() &&
                          plan->components[result.id - 4].phase == LoadPhase::Deferred;
    std::optional<LoadDiagnostic> diagnostic;
    if (result.error) {
        auto source = plan ? plan->source : ComponentSource{"master", request->entry.string(), {}};
        if (result.id >= 4 && result.id - 4 < units.size()) {
            const auto &unit = plan->components[result.id - 4];
            source = {unit.id, (plan->package_root / unit.source_path).string(), {}};
        }
        diagnostic =
            LoadDiagnostic{result.error->code == TaskErrorCode::Cancelled ? LoadStage::Cancelled
                                                                          : LoadStage::Semantic,
                           std::move(source), 0, result.error->message};
    } else if (!output) {
        diagnostic = LoadDiagnostic{LoadStage::Semantic, plan ? plan->source : ComponentSource{}, 0,
                                    "Invalid component work result"};
    } else {
        timings.read_us += output->timings.read_us;
        timings.prepare_us += output->timings.prepare_us;
        stats.source_bytes += output->source_bytes;
        diagnostic = output->diagnostic;
        if (stats.source_bytes > kMaxLoadSourceBytes) {
            diagnostic = LoadDiagnostic{LoadStage::Read, plan ? plan->source : ComponentSource{}, 0,
                                        "Load graph exceeds 8 MiB source budget"};
        }
    }
    if (diagnostic) {
        if (deferred) {
            deferred_diagnostics.push_back(std::move(*diagnostic));
            units[result.id - 4].complete = true;
            region_completions.push_back({request->load,
                                          plan->components[result.id - 4].id,
                                          {},
                                          deferred_diagnostics.back()});
        } else {
            Fail(std::move(*diagnostic));
        }
        return;
    }

    if (result.id == 1) {
        plan_owner = result.output;
        plan = output->plan ? std::shared_ptr<const LoadPlan>(result.output, output->plan.get())
                            : nullptr;
        if (!plan) {
            Fail({LoadStage::Semantic, {}, 0, "Missing load plan"});
            return;
        }
        stats.component_count = plan->components.size();
        if (plan->legacy) {
            stats.critical_prepared = 1;
            stats.critical_ready = true;
            completion = MasterLoadCompletion{
                request->load, plan, output->prepared->WithRetention(result.output), {}, timings};
        } else {
            units.resize(plan->components.size());
        }
    } else if (result.id == 2) {
        layout_owner = result.output;
        layout = output->layout->WithRetention(result.output);
    } else if (result.id == 3) {
        stats.critical_ready = true;
        completion = MasterLoadCompletion{
            request->load, plan, output->prepared->WithRetention(result.output), {}, timings};
    } else {
        const auto i = result.id - 4;
        units[i].complete = true;
        units[i].prepared = output->prepared->WithRetention(result.output);
        if (deferred) {
            ++stats.deferred_prepared;
            region_completions.push_back(
                {request->load, plan->components[i].id, units[i].prepared, {}});
        } else {
            ++stats.critical_prepared;
        }
    }
}

void MasterLoadSession::Impl::Drain()
{
    while (auto result = channel->TakeCompletion()) {
        Complete(std::move(*result));
    }
    Dispatch();
}

std::optional<MasterLoadCompletion> MasterLoadSession::TakeCompletion()
{
    auto &self = *impl_;
    self.Drain();
    auto result = std::move(self.completion);
    self.completion.reset();
    self.delivered |= result.has_value();
    return result;
}

std::optional<MasterRegionCompletion> MasterLoadSession::TakeRegionCompletion()
{
    auto &self = *impl_;
    self.Drain();
    if (self.failed || self.stopped || self.region_completions.empty()) {
        return std::nullopt;
    }

    auto result = std::move(self.region_completions.front());
    self.region_completions.pop_front();
    return result;
}

void MasterLoadSession::MasterPresented(UiLoadId load)
{
    auto &self = *impl_;
    if (!self.request || self.request->load != load || !self.stats.critical_ready || self.failed ||
        self.stopped || self.stats.deferred_started) {
        return;
    }
    self.stats.deferred_started = true;
    self.Dispatch();
}

void MasterLoadSession::Cancel(UiLoadId load)
{
    auto &self = *impl_;
    if (!self.request || self.request->load != load || self.stopped) {
        return;
    }
    if (self.delivered) {
        self.failed = true;
        self.CancelTasks();
        self.region_completions.clear();
        return;
    }

    self.Fail({LoadStage::Cancelled,
               {"master", self.request->entry.string(), {}},
               0,
               "Master loading cancelled"});
}

void MasterLoadSession::Stop()
{
    auto &self = *impl_;
    if (self.stopped) {
        return;
    }
    self.stopped = true;
    self.channel->Stop();
    self.completion.reset();
    self.region_completions.clear();
    self.units.clear();
    self.layout.reset();
    self.plan.reset();
    self.layout_owner.reset();
    self.plan_owner.reset();
}

int MasterLoadSession::Fd() const noexcept
{
    return impl_->channel->Fd();
}

MasterLoadStats MasterLoadSession::Stats() const
{
    return impl_->stats;
}

std::span<const LoadDiagnostic> MasterLoadSession::DeferredDiagnostics() const
{
    return impl_->deferred_diagnostics;
}
} // namespace prism::runtime
