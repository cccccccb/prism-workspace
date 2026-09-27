#include "prism/runtime/load_session.hpp"
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>
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

struct LoadSession::Impl {
    struct Job {
        LoadRequest request;
        std::stop_source cancellation;
    };

    explicit Impl(PrepareFunction value) : prepare(value ? std::move(value) : PrepareDefault)
    {
        completion_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (completion_fd < 0) {
            throw std::system_error(errno, std::generic_category(), "UI completion eventfd");
        }
    }

    ~Impl()
    {
        close(completion_fd);
    }

    bool HasJob() const
    {
        return stopped || pending.has_value();
    }

    bool CanPublish() const
    {
        return stopped || !completed.has_value();
    }

    void Signal()
    {
        const std::uint64_t one = 1;
        while (write(completion_fd, &one, sizeof(one)) < 0 && errno == EINTR) {
        }
    }

    void Drain()
    {
        std::uint64_t value;
        while (read(completion_fd, &value, sizeof(value)) < 0 && errno == EINTR) {
        }
    }

    LoadCompletion Execute(const Job &job)
    {
        LoadCompletion result{job.request.load, job.request.source, {}, {}, {}};
        const auto token = job.cancellation.get_token();
        auto phase_start = Clock::now();
        bool preparing = false;
        try {
            auto text = ReadSource(job.request, token);
            result.timings.read_us = ElapsedUs(phase_start);
            CheckCancelled(job.request.source, token);

            phase_start = Clock::now();
            preparing = true;
            result.prepared = prepare(text, job.request.source, token);
            result.timings.prepare_us = ElapsedUs(phase_start);
            CheckCancelled(job.request.source, token);
            if (!*result.prepared) {
                throw LoadFailure({LoadStage::Semantic, job.request.source, 0,
                                   "Compiler returned an invalid prepared component"});
            }
        } catch (const LoadFailure &error) {
            result.diagnostic = error.Diagnostic();
        } catch (const std::exception &error) {
            result.diagnostic = LoadDiagnostic{preparing ? LoadStage::Semantic : LoadStage::Read,
                                               job.request.source, 0, error.what()};
        } catch (...) {
            result.diagnostic =
                LoadDiagnostic{preparing ? LoadStage::Semantic : LoadStage::Read,
                               job.request.source, 0, "Unknown UI preparation exception"};
        }

        if (preparing) {
            result.timings.prepare_us = ElapsedUs(phase_start);
        } else {
            result.timings.read_us = ElapsedUs(phase_start);
        }
        if (token.stop_requested()) {
            result.diagnostic = CancelledDiagnostic(job.request.source);
        }
        if (result.diagnostic) {
            result.prepared.reset();
        }
        return result;
    }

    void Run()
    {
        for (;;) {
            std::optional<Job> job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, std::bind_front(&Impl::HasJob, this));
                if (stopped) {
                    return;
                }
                job = std::move(pending);
                pending.reset();
                active_load = job->request.load;
                active_cancellation = job->cancellation;
            }

            auto result = Execute(*job);
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, std::bind_front(&Impl::CanPublish, this));
                if (stopped) {
                    return;
                }
                // Cancel can race compilation or a result waiting for the
                // single completion slot. Check again at publication.
                if (job->cancellation.stop_requested()) {
                    result.prepared.reset();
                    result.diagnostic = CancelledDiagnostic(job->request.source);
                }
                completed = std::move(result);
                active_load = {};
                active_cancellation.reset();
                Signal();
            }
        }
    }

    PrepareFunction prepare;
    int completion_fd{-1};
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    std::optional<Job> pending;
    std::optional<LoadCompletion> completed;
    std::optional<std::stop_source> active_cancellation;
    UiLoadId active_load{};
    std::size_t outstanding{};
    bool stopped{false};
};

LoadSession::LoadSession(PrepareFunction prepare)
    : impl_(std::make_unique<Impl>(std::move(prepare)))
{
}

LoadSession::~LoadSession()
{
    Stop();
}

LoadSubmitResult LoadSession::Submit(LoadRequest request)
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) {
        return LoadSubmitResult::Closed;
    }
    if (request.load.owner == 0 || request.load.generation == 0 || request.path.empty() ||
        request.path.find('\0') != std::string::npos) {
        return LoadSubmitResult::Invalid;
    }
    if (impl_->outstanding >= 2 || impl_->pending) {
        return LoadSubmitResult::Busy;
    }
    if (request.source.source_path.empty()) {
        request.source.source_path = request.path;
    }

    impl_->pending = Impl::Job{std::move(request), {}};
    ++impl_->outstanding;
    try {
        if (!impl_->worker.joinable()) {
            impl_->worker = std::thread(&Impl::Run, impl_.get());
        }
    } catch (...) {
        impl_->pending.reset();
        --impl_->outstanding;
        throw;
    }
    impl_->wake.notify_one();
    return LoadSubmitResult::Accepted;
}

void LoadSession::Cancel(UiLoadId load)
{
    std::optional<std::stop_source> active;
    std::optional<std::stop_source> pending;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->active_load == load) {
            active = impl_->active_cancellation;
        }
        if (impl_->pending && impl_->pending->request.load == load) {
            pending = impl_->pending->cancellation;
        }
        if (impl_->completed && impl_->completed->load == load) {
            impl_->completed->prepared.reset();
            impl_->completed->diagnostic = CancelledDiagnostic(impl_->completed->source);
        }
    }
    if (active) {
        active->request_stop();
    }
    if (pending) {
        pending->request_stop();
    }
    {
        std::lock_guard lock(impl_->mutex);
        // The worker may have published between copying its stop source and
        // requesting cancellation. Owner consumption cannot race this call.
        if (impl_->completed && impl_->completed->load == load) {
            impl_->completed->prepared.reset();
            impl_->completed->diagnostic = CancelledDiagnostic(impl_->completed->source);
        }
    }
    impl_->wake.notify_one();
}

void LoadSession::Stop()
{
    std::optional<std::stop_source> active;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopped = true;
        active = impl_->active_cancellation;
        impl_->pending.reset();
        impl_->completed.reset();
        impl_->outstanding = 0;
        impl_->Drain();
    }
    if (active) {
        active->request_stop();
    }
    impl_->wake.notify_one();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

int LoadSession::Fd() const noexcept
{
    return impl_->completion_fd;
}

std::optional<LoadCompletion> LoadSession::TakeCompletion()
{
    std::optional<LoadCompletion> result;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->Drain();
        if (impl_->completed) {
            result = std::move(impl_->completed);
            impl_->completed.reset();
            --impl_->outstanding;
        }
    }
    impl_->wake.notify_one();
    return result;
}

} // namespace prism::runtime
