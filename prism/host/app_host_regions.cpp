#include "app_host_p.hpp"

namespace prism::sdk {
using namespace host_detail;

void AppHost::Impl::TakeRegionCompletions()
{
    while (auto completion = master_loader->TakeRegionCompletion()) {
        if (completion->load != ui.master_load || failed || closed) {
            continue;
        }
        if (rejected_regions.contains(completion->component) ||
            installed_regions.contains(completion->component)) {
            continue;
        }
        if (completion->diagnostic) {
            // Preparation diagnostics already belong to the graph session.
            rejected_regions.insert(completion->component);
        } else if (completion->prepared) {
            pending_regions.insert_or_assign(completion->component,
                                             std::move(*completion->prepared));
        }
    }
}

void AppHost::Impl::RejectRegion(std::string_view name, runtime::LoadDiagnostic diagnostic)
{
    if (rejected_regions.emplace(name).second) {
        region_diagnostics.push_back(std::move(diagnostic));
    }
    pending_regions.erase(std::string(name));
}

void AppHost::Impl::RejectDependentRegions()
{
    bool changed;
    do {
        changed = false;
        for (const auto &unit : installed_plan->components) {
            if (unit.phase != runtime::LoadPhase::Deferred || rejected_regions.contains(unit.id) ||
                installed_regions.contains(unit.id)) {
                continue;
            }
            for (const auto &dependency : unit.after) {
                if (rejected_regions.contains(dependency)) {
                    RejectRegion(
                        unit.id,
                        {runtime::LoadStage::Install,
                         {unit.id, (installed_plan->package_root / unit.source_path).string(), {}},
                         unit.line,
                         "Region dependency could not be installed: " + dependency});
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);
}

bool AppHost::Impl::RegionsNeedWork() const
{
    if (!ui.master_presented || !installed_plan || installed_plan->legacy ||
        !installing_regions.empty()) {
        return false;
    }
    for (const auto &unit : installed_plan->components) {
        if (rejected_regions.contains(unit.id) || installed_regions.contains(unit.id) ||
            !pending_regions.contains(unit.id)) {
            continue;
        }
        bool eligible = true;
        for (const auto &dependency : unit.after) {
            const auto *parent = runtime::FindLoadUnit(*installed_plan, dependency);
            eligible &= parent && (parent->phase == runtime::LoadPhase::Critical ||
                                   installed_regions.contains(dependency));
        }
        if (eligible) {
            return true;
        }
    }
    return false;
}

bool AppHost::Impl::InstallRegions()
{
    if (!ui.master_presented || !installed_plan || installed_plan->legacy || failed || closed) {
        return true;
    }
    RejectDependentRegions();

    if (installing_regions.empty()) {
        std::set<std::string, std::less<>> selected;
        bool added;
        do {
            added = false;
            for (const auto &unit : installed_plan->components) {
                if (installing_regions.size() >= region_batch_limit || selected.contains(unit.id) ||
                    rejected_regions.contains(unit.id) || installed_regions.contains(unit.id) ||
                    !pending_regions.contains(unit.id)) {
                    continue;
                }
                bool eligible = true;
                for (const auto &dependency : unit.after) {
                    const auto *parent = runtime::FindLoadUnit(*installed_plan, dependency);
                    eligible &= parent && (parent->phase == runtime::LoadPhase::Critical ||
                                           installed_regions.contains(dependency) ||
                                           selected.contains(dependency));
                }
                if (eligible) {
                    installing_regions.push_back({unit.id, pending_regions.at(unit.id)});
                    selected.insert(unit.id);
                    added = true;
                }
            }
        } while (added && installing_regions.size() < region_batch_limit);
        if (installing_regions.empty()) {
            return true;
        }

        runtime::LoadDiagnostic diagnostic;
        if (!frontend->StartRegionInstall(ui.master_load, installing_regions, &diagnostic)) {
            // A combined limit can reject an otherwise valid collection. Retry
            // individually before assigning a local failure to any component.
            if (installing_regions.size() > 1) {
                region_batch_limit = 1;
            } else {
                RejectRegion(installing_regions.front().region, std::move(diagnostic));
            }
            installing_regions.clear();
            return true;
        }
    }
    if (install_advanced) {
        return true;
    }
    install_advanced = true;
    runtime::LoadDiagnostic diagnostic;
    const auto state = frontend->AdvanceUiInstall(bindings, &diagnostic);
    if (state == runtime::UiInstallState::Pending) {
        return true;
    }
    if (state == runtime::UiInstallState::Committed) {
        for (const auto &region : installing_regions) {
            installed_regions.insert(region.region);
            CollectBindings(region.prepared.Root(), mounted_bindings);
            pending_regions.erase(region.region);
        }
    } else if (installing_regions.size() > 1) {
        region_batch_limit = 1;
    } else {
        const auto &region = installing_regions.front();
        if (diagnostic.source.component_id.empty()) {
            diagnostic.source = region.prepared.Source();
        }
        RejectRegion(region.region, std::move(diagnostic));
    }
    installing_regions.clear();
    return true;
}
} // namespace prism::sdk
