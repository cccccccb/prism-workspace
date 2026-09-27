#pragma once

#include "prism/sdk/module_session.hpp"
#include <vector>

namespace prism::sdk {
struct ModuleWorkState {
    struct Pending {
        std::size_t input_bytes{};
        bool cancelled{};
    };

    std::shared_ptr<runtime::TaskScheduler> scheduler;
    std::shared_ptr<runtime::TaskChannel> channel;
    std::map<std::uint64_t, Pending> pending;
    std::size_t input_bytes{};
};

struct ModuleWorkOutput final : runtime::TaskOutput {
    ModuleWorkOutput(std::uint32_t state, std::int32_t code, std::vector<std::uint8_t> bytes,
                     std::string message);
    std::uint64_t RetainedBytes() const noexcept override;
    const std::uint32_t status;
    const std::int32_t error_code;
    const std::vector<std::uint8_t> result;
    const std::string detail;
};

class ModuleWorkJob {
public:
    ModuleWorkJob(PrismWorkFunctionV1 function, std::vector<std::uint8_t> input,
                  std::uint64_t maximum_result);
    std::shared_ptr<const runtime::TaskOutput> operator()(std::stop_token stop) const;

private:
    PrismWorkFunctionV1 function_;
    std::vector<std::uint8_t> input_;
    std::uint64_t maximum_result_;
};
} // namespace prism::sdk
