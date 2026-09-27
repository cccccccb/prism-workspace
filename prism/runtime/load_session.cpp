#include "prism/runtime/load_session.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace prism::runtime {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_source_bytes = 1024 * 1024;

class SourceFile {
public:
    explicit SourceFile(int fd) : fd_(fd)
    {
    }

    ~SourceFile()
    {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    SourceFile(const SourceFile &) = delete;
    SourceFile &operator=(const SourceFile &) = delete;

    int Fd() const
    {
        return fd_;
    }

private:
    int fd_;
};

std::uint64_t ElapsedUs(Clock::time_point start)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

[[noreturn]] void ReadError(const LoadRequest &request, std::string message)
{
    throw LoadFailure({LoadStage::Read, request.source, 0, std::move(message)});
}

void CheckCancelled(const ComponentSource &source, std::stop_token stop)
{
    if (stop.stop_requested()) {
        throw LoadFailure({LoadStage::Cancelled, source, 0, "UI preparation cancelled"});
    }
}

PreparedComponent PrepareDefault(std::string_view text, ComponentSource source,
                                 std::stop_token stop)
{
    CheckCancelled(source, stop);
    auto result = PrepareComponent(text, source);
    CheckCancelled(source, stop);
    return result;
}

std::string ReadSource(const LoadRequest &request, std::stop_token stop)
{
    CheckCancelled(request.source, stop);
    struct stat attributes{};
    if (stat(request.path.c_str(), &attributes) != 0) {
        ReadError(request, "Cannot inspect UI source: " + std::string(std::strerror(errno)));
    }
    if (!S_ISREG(attributes.st_mode)) {
        ReadError(request, "UI source must be a regular file");
    }

    // Validate before opening devices/FIFOs, then recheck the actual opened
    // descriptor. NONBLOCK also protects a path replaced with a FIFO meanwhile.
    SourceFile file(open(request.path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY));
    if (file.Fd() < 0) {
        ReadError(request, "Cannot open UI source: " + std::string(std::strerror(errno)));
    }
    if (fstat(file.Fd(), &attributes) != 0) {
        ReadError(request, "Cannot inspect UI source: " + std::string(std::strerror(errno)));
    }
    if (!S_ISREG(attributes.st_mode)) {
        ReadError(request, "UI source must be a regular file");
    }
    if (attributes.st_size < 0 ||
        static_cast<std::uint64_t>(attributes.st_size) > max_source_bytes) {
        ReadError(request, "UI source exceeds 1 MiB limit");
    }

    std::string text;
    text.reserve(static_cast<std::size_t>(attributes.st_size));
    char bytes[4096];
    for (;;) {
        CheckCancelled(request.source, stop);
        const auto size = read(file.Fd(), bytes, sizeof(bytes));
        if (size < 0) {
            if (errno == EINTR) {
                continue;
            }
            ReadError(request, "Cannot read UI source: " + std::string(std::strerror(errno)));
        }
        if (size == 0) {
            break;
        }
        if (static_cast<std::size_t>(size) > max_source_bytes - text.size()) {
            ReadError(request, "UI source exceeds 1 MiB limit");
        }
        text.append(bytes, static_cast<std::size_t>(size));
    }
    CheckCancelled(request.source, stop);
    if (text.empty()) {
        ReadError(request, "UI source is empty");
    }
    return text;
}

LoadDiagnostic CancelledDiagnostic(const ComponentSource &source)
{
    return {LoadStage::Cancelled, source, 0, "UI preparation cancelled"};
}
} // namespace

namespace {
struct LoadOutput : TaskOutput {
    explicit LoadOutput(LoadCompletion value) : completion(std::move(value))
    {
    }

    std::uint64_t RetainedBytes() const noexcept override
    {
        auto bytes = sizeof(LoadOutput) + 256 + completion.source.component_id.capacity() +
                     completion.source.source_path.capacity() +
                     completion.source.source_version.capacity();
        if (completion.prepared) {
            bytes += completion.prepared->RetainedBytes();
        }
        if (completion.diagnostic) {
            const auto &diagnostic = *completion.diagnostic;
            bytes += diagnostic.message.capacity() + diagnostic.source.component_id.capacity() +
                     diagnostic.source.source_path.capacity() +
                     diagnostic.source.source_version.capacity();
        }
        return bytes;
    }

    LoadCompletion completion;
};

struct LoadWork {
    LoadRequest request;
    PrepareFunction prepare;

    std::shared_ptr<const TaskOutput> operator()(std::stop_token token) const
    {
        LoadCompletion result{request.load, request.source, {}, {}, {}};
        auto phase_start = Clock::now();
        bool preparing = false;
        try {
            auto text = ReadSource(request, token);
            result.timings.read_us = ElapsedUs(phase_start);
            CheckCancelled(request.source, token);

            phase_start = Clock::now();
            preparing = true;
            result.prepared = prepare(text, request.source, token);
            result.timings.prepare_us = ElapsedUs(phase_start);
            CheckCancelled(request.source, token);
            if (!*result.prepared) {
                throw LoadFailure({LoadStage::Semantic, request.source, 0,
                                   "Compiler returned an invalid prepared component"});
            }
        } catch (const LoadFailure &error) {
            result.diagnostic = error.Diagnostic();
        } catch (const std::exception &error) {
            result.diagnostic = LoadDiagnostic{preparing ? LoadStage::Semantic : LoadStage::Read,
                                               request.source, 0, error.what()};
        } catch (...) {
            result.diagnostic =
                LoadDiagnostic{preparing ? LoadStage::Semantic : LoadStage::Read, request.source, 0,
                               "Unknown UI preparation exception"};
        }

        if (preparing) {
            result.timings.prepare_us = ElapsedUs(phase_start);
        } else {
            result.timings.read_us = ElapsedUs(phase_start);
        }
        if (token.stop_requested()) {
            result.diagnostic = CancelledDiagnostic(request.source);
        }
        if (result.diagnostic) {
            result.prepared.reset();
        }
        return std::make_shared<const LoadOutput>(std::move(result));
    }
};
} // namespace

struct LoadSession::Impl {
    Impl(PrepareFunction value, std::shared_ptr<TaskScheduler> shared_scheduler)
        : prepare(value ? std::move(value) : PrepareDefault),
          scheduler(shared_scheduler ? std::move(shared_scheduler)
                                     : std::make_shared<TaskScheduler>()),
          channel(scheduler->OpenChannel(2, 1, false))
    {
    }

    PrepareFunction prepare;
    std::shared_ptr<TaskScheduler> scheduler;
    std::shared_ptr<TaskChannel> channel;
    std::unordered_map<std::uint64_t, LoadRequest> requests;
    std::uint64_t next_id{1};
    bool stopped{};
};

LoadSession::LoadSession(PrepareFunction prepare, std::shared_ptr<TaskScheduler> scheduler)
    : impl_(std::make_unique<Impl>(std::move(prepare), std::move(scheduler)))
{
}

LoadSession::~LoadSession()
{
    Stop();
}

LoadSubmitResult LoadSession::Submit(LoadRequest request)
{
    if (impl_->stopped) {
        return LoadSubmitResult::Closed;
    }
    if (!request.load.owner || !request.load.generation || request.path.empty() ||
        request.path.find('\0') != std::string::npos ||
        impl_->next_id == std::numeric_limits<std::uint64_t>::max()) {
        return LoadSubmitResult::Invalid;
    }
    if (request.source.source_path.empty()) {
        request.source.source_path = request.path;
    }

    const auto id = impl_->next_id;
    impl_->requests.emplace(id, request);
    TaskSubmitResult result;
    try {
        result = impl_->channel->Submit({id, TaskPriority::Critical, 32ULL * 1024 * 1024,
                                         LoadWork{std::move(request), impl_->prepare}});
    } catch (...) {
        impl_->requests.erase(id);
        throw;
    }
    if (result != TaskSubmitResult::Accepted) {
        impl_->requests.erase(id);
        switch (result) {
        case TaskSubmitResult::Busy:
            return LoadSubmitResult::Busy;
        case TaskSubmitResult::Closed:
            return LoadSubmitResult::Closed;
        default:
            return LoadSubmitResult::Invalid;
        }
    }
    ++impl_->next_id;
    return LoadSubmitResult::Accepted;
}

void LoadSession::Cancel(UiLoadId load)
{
    for (const auto &[id, request] : impl_->requests) {
        if (request.load == load) {
            impl_->channel->Cancel(id);
        }
    }
}

void LoadSession::Stop()
{
    impl_->stopped = true;
    impl_->channel->Stop();
    impl_->requests.clear();
}

int LoadSession::Fd() const noexcept
{
    return impl_->channel->Fd();
}

std::optional<LoadCompletion> LoadSession::TakeCompletion()
{
    const auto task = impl_->channel->TakeCompletion();
    if (!task) {
        return {};
    }
    const auto request = impl_->requests.find(task->id);
    if (request == impl_->requests.end()) {
        throw std::logic_error("Unknown load task completion");
    }
    LoadCompletion result{request->second.load, request->second.source, {}, {}, {}};
    impl_->requests.erase(request);
    if (task->error) {
        result.diagnostic =
            LoadDiagnostic{task->error->code == TaskErrorCode::Cancelled ? LoadStage::Cancelled
                                                                         : LoadStage::Semantic,
                           result.source, 0, task->error->message};
        return result;
    }
    const auto output = std::dynamic_pointer_cast<const LoadOutput>(task->output);
    if (!output) {
        result.diagnostic =
            LoadDiagnostic{LoadStage::Semantic, result.source, 0, "Unexpected load task output"};
        return result;
    }
    result = output->completion;
    if (result.prepared) {
        result.prepared = result.prepared->WithRetention(output);
    }
    return result;
}

} // namespace prism::runtime
