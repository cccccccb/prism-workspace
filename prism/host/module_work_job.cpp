#include "module_work_p.hpp"
#include <cerrno>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

namespace prism::sdk {
namespace {
class CancellationFd {
public:
    CancellationFd()
    {
        fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "Business cancellation eventfd");
        }
    }

    ~CancellationFd()
    {
        close(fd_);
    }

    int Fd() const noexcept
    {
        return fd_;
    }

private:
    int fd_{-1};
};

struct StopNotifier {
    int fd;

    void operator()() const noexcept
    {
        const std::uint64_t one = 1;
        while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
        }
    }
};

class WorkContext {
public:
    WorkContext(std::stop_token stop, std::uint64_t maximum)
        : stop_(stop), maximum_(maximum), notifier_(stop, StopNotifier{cancellation_.Fd()})
    {
    }

    PrismWorkContextV1 Api()
    {
        return {sizeof(PrismWorkContextV1), this, IsCancelled, SetResult, cancellation_.Fd()};
    }

    std::shared_ptr<const ModuleWorkOutput> Finish(std::int32_t code)
    {
        if (stop_.stop_requested()) {
            return std::make_shared<const ModuleWorkOutput>(PRISM_WORK_CANCELLED_V1, 0,
                                                            std::vector<std::uint8_t>{},
                                                            "Business preparation cancelled");
        }
        if (failed_ || code) {
            return std::make_shared<const ModuleWorkOutput>(
                PRISM_WORK_FAILED_V1, code ? code : -1, std::vector<std::uint8_t>{},
                failed_ ? "Business result writer rejected its result"
                        : "Business preparation returned an error");
        }
        return std::make_shared<const ModuleWorkOutput>(PRISM_WORK_SUCCEEDED_V1, 0,
                                                        std::move(result_), std::string{});
    }

private:
    static std::int32_t IsCancelled(void *context) noexcept
    {
        return static_cast<WorkContext *>(context)->stop_.stop_requested() ? 1 : 0;
    }

    static std::int32_t SetResult(void *context, PrismBytesViewV1 value) noexcept
    {
        auto &self = *static_cast<WorkContext *>(context);
        if (std::this_thread::get_id() != self.worker_thread_) {
            return -1;
        }
        if (self.failed_ || self.written_ || self.stop_.stop_requested() ||
            value.size > self.maximum_ || (!value.data && value.size)) {
            self.failed_ = true;
            return -1;
        }

        self.written_ = true;
        try {
            if (value.size) {
                self.result_.assign(value.data, value.data + value.size);
            }
            return 0;
        } catch (...) {
            self.failed_ = true;
            return -1;
        }
    }

    const std::thread::id worker_thread_{std::this_thread::get_id()};
    std::stop_token stop_;
    std::uint64_t maximum_;
    CancellationFd cancellation_;
    // Destroy the stop callback before closing its eventfd.
    std::stop_callback<StopNotifier> notifier_;
    std::vector<std::uint8_t> result_;
    bool written_{}, failed_{};
};
} // namespace

ModuleWorkOutput::ModuleWorkOutput(std::uint32_t state, std::int32_t code,
                                   std::vector<std::uint8_t> bytes, std::string message)
    : status(state), error_code(code), result(std::move(bytes)), detail(std::move(message))
{
}

std::uint64_t ModuleWorkOutput::RetainedBytes() const noexcept
{
    return sizeof(*this) + result.capacity() + detail.capacity();
}

ModuleWorkJob::ModuleWorkJob(PrismWorkFunctionV1 function, std::vector<std::uint8_t> input,
                             std::uint64_t maximum_result)
    : function_(function), input_(std::move(input)), maximum_result_(maximum_result)
{
}

std::shared_ptr<const runtime::TaskOutput> ModuleWorkJob::operator()(std::stop_token stop) const
{
    WorkContext context(stop, maximum_result_);
    auto api = context.Api();
    const PrismBytesViewV1 input{input_.data(), input_.size()};
    // The only executable module reference is the immutable work function.
    // The ModuleSession keeps its library loaded until this channel has joined.
    const auto code = function_(&api, input);
    return context.Finish(code);
}
} // namespace prism::sdk
