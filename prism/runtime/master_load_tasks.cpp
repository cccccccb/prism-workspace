#include "master_load_session_p.hpp"
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace prism::runtime {
namespace {
using Clock = std::chrono::steady_clock;

std::uint64_t Elapsed(Clock::time_point started)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
}

void CheckStop(std::stop_token stop, const ComponentSource &source)
{
    if (stop.stop_requested()) {
        throw LoadFailure({LoadStage::Cancelled, source, 0, "Component loading cancelled"});
    }
}

struct ReadFd {
    int value{-1};

    ~ReadFd()
    {
        if (value >= 0) {
            close(value);
        }
    }
};

std::string Read(const MasterReadWork &work, std::stop_token stop)
{
    CheckStop(stop, work.source);
    try {
        const auto root = std::filesystem::canonical(work.root);
        const auto path = std::filesystem::canonical(work.file);
        auto a = root.begin(), b = path.begin();
        for (; a != root.end() && b != path.end() && *a == *b; ++a, ++b) {
        }
        if (a != root.end()) {
            throw std::runtime_error("Component source escapes package root");
        }
        ReadFd fd{open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW)};
        struct stat info{};
        if (fd.value < 0 || fstat(fd.value, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size <= 0 || static_cast<std::uint64_t>(info.st_size) > kMaxLoadFileBytes) {
            throw std::runtime_error("Component source must be a regular file within 1 MiB");
        }
        const auto opened = std::filesystem::canonical("/proc/self/fd/" + std::to_string(fd.value));
        auto root_part = root.begin(), opened_part = opened.begin();
        for (; root_part != root.end() && opened_part != opened.end() && *root_part == *opened_part;
             ++root_part, ++opened_part) {
        }
        if (root_part != root.end()) {
            throw std::runtime_error("Opened component source escapes package root");
        }
        std::string source;
        source.reserve(info.st_size);
        char buffer[4096];
        for (;;) {
            CheckStop(stop, work.source);
            const auto bytes = read(fd.value, buffer, sizeof(buffer));
            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            if (bytes < 0) {
                throw std::runtime_error("Cannot read component source");
            }
            if (bytes == 0) {
                break;
            }
            if (source.size() + bytes > kMaxLoadFileBytes) {
                throw std::runtime_error("Component source exceeds 1 MiB");
            }
            source.append(buffer, bytes);
        }
        struct stat after{};
        if (source.size() != static_cast<std::size_t>(info.st_size) ||
            fstat(fd.value, &after) != 0 || after.st_size != info.st_size ||
            after.st_mtim.tv_sec != info.st_mtim.tv_sec ||
            after.st_mtim.tv_nsec != info.st_mtim.tv_nsec ||
            after.st_ctim.tv_sec != info.st_ctim.tv_sec ||
            after.st_ctim.tv_nsec != info.st_ctim.tv_nsec) {
            throw std::runtime_error("Component source changed during loading");
        }
        return source;
    } catch (const LoadFailure &) {
        throw;
    } catch (const std::exception &error) {
        throw LoadFailure({LoadStage::Read, work.source, 0, error.what()});
    }
}

std::size_t PlanBytes(const LoadPlan &plan)
{
    std::size_t bytes = sizeof(LoadPlan) + plan.components.capacity() * sizeof(LoadUnit) +
                        plan.bindings.capacity() * sizeof(LoadBinding);
    for (const auto &unit : plan.components) {
        bytes += unit.id.capacity() + unit.source_path.native().capacity() +
                 unit.after.capacity() * sizeof(std::string);
        for (const auto &dependency : unit.after) {
            bytes += dependency.capacity();
        }
    }
    for (const auto &binding : plan.bindings) {
        bytes += binding.name.capacity();
        if (const auto *text = std::get_if<std::string>(&binding.initial)) {
            bytes += text->capacity();
        }
    }
    return bytes + plan.source.component_id.capacity() + plan.source.source_path.capacity() +
           plan.source.source_version.capacity() + plan.package_root.native().capacity() +
           plan.layout_path.native().capacity();
}

void ValidateSourceBudget(const LoadPlan &plan)
{
    if (plan.legacy) {
        return;
    }
    auto total = plan.source_bytes;
    std::vector<std::pair<std::filesystem::path, bool>> files{{plan.layout_path, true}};
    for (const auto &unit : plan.components) {
        files.emplace_back(unit.source_path, unit.phase == LoadPhase::Critical);
    }
    const auto root = std::filesystem::canonical(plan.package_root);
    for (const auto &[relative, required] : files) {
        std::error_code error;
        const auto path = std::filesystem::canonical(root / relative, error);
        if (error) {
            if (required) {
                throw LoadFailure(
                    {LoadStage::Read, plan.source, 0, "Missing required layout/component file"});
            }
            continue;
        }
        auto a = root.begin(), b = path.begin();
        for (; a != root.end() && b != path.end() && *a == *b; ++a, ++b) {
        }
        if (a != root.end()) {
            throw LoadFailure(
                {LoadStage::Read, plan.source, 0, "Component source escapes package root"});
        }
        struct stat info{};
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
            static_cast<std::uint64_t>(info.st_size) > kMaxLoadFileBytes) {
            if (required) {
                throw LoadFailure({LoadStage::Read, plan.source, 0,
                                   "Required component file is outside source limits"});
            }
            continue;
        }
        total += info.st_size;
        if (total > kMaxLoadSourceBytes) {
            throw LoadFailure(
                {LoadStage::Read, plan.source, 0, "Load graph exceeds 8 MiB source budget"});
        }
    }
}
} // namespace

std::uint64_t MasterTaskOutput::RetainedBytes() const noexcept
{
    return sizeof(MasterTaskOutput) + (plan ? PlanBytes(*plan) : 0) +
           (prepared ? prepared->RetainedBytes() : 0) + (layout ? layout->RetainedBytes() : 0) +
           (diagnostic
                ? diagnostic->message.capacity() + diagnostic->source.component_id.capacity() +
                      diagnostic->source.source_path.capacity() +
                      diagnostic->source.source_version.capacity()
                : 0);
}

std::shared_ptr<const TaskOutput> MasterReadWork::operator()(std::stop_token stop) const
{
    auto output = std::make_shared<MasterTaskOutput>();
    try {
        auto started = Clock::now();
        const auto text = Read(*this, stop);
        output->timings.read_us = Elapsed(started);
        output->source_bytes = text.size();
        started = Clock::now();
        CheckStop(stop, source);
        if (entry) {
            if (IsInterfaceSource(text, source)) {
                output->plan = std::make_shared<LoadPlan>(CompileLoadPlan(text, source, root));
                ValidateSourceBudget(*output->plan);
            } else {
                output->prepared =
                    prepare ? prepare(text, source, stop) : PrepareComponent(text, source);
                output->plan = std::make_shared<LoadPlan>(
                    CompileLegacyLoadPlan(*output->prepared, source, root));
            }
        } else if (layout) {
            output->layout = PrepareLayout(text, *plan, source);
        } else {
            output->prepared =
                prepare ? prepare(text, source, stop) : PrepareComponent(text, source);
            ValidatePreparedUnit(*plan, source.component_id, *output->prepared);
        }
        CheckStop(stop, source);
        output->timings.prepare_us = Elapsed(started);
    } catch (const LoadFailure &error) {
        output->diagnostic = error.Diagnostic();
    } catch (const std::exception &error) {
        output->diagnostic = LoadDiagnostic{LoadStage::Semantic, source, 0, error.what()};
    }
    return output;
}

std::shared_ptr<const TaskOutput> MasterComposeWork::operator()(std::stop_token stop) const
{
    auto output = std::make_shared<MasterTaskOutput>();
    try {
        CheckStop(stop, plan->source);
        const auto started = Clock::now();
        output->prepared = ComposeCritical(*plan, layout, units);
        CheckStop(stop, plan->source);
        output->timings.prepare_us = Elapsed(started);
    } catch (const LoadFailure &error) {
        output->diagnostic = error.Diagnostic();
    } catch (const std::exception &error) {
        output->diagnostic = LoadDiagnostic{LoadStage::Semantic, plan->source, 0, error.what()};
    }
    return output;
}
} // namespace prism::runtime
