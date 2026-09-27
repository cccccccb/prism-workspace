#pragma once
#include "prism/contracts/app_module.h"
#include <cstdint>
#include <filesystem>

namespace prism::launch {
// Loader lifetime must outlive every instance created through Api().
class AppModule {
public:
    explicit AppModule(const std::filesystem::path &file);
    ~AppModule();
    AppModule(const AppModule &) = delete;
    AppModule &operator=(const AppModule &) = delete;

    const PrismAppModuleV1 &Api() const
    {
        return api_;
    }

    std::uint64_t LoadDurationNs() const noexcept
    {
        return load_duration_ns_;
    }

private:
    void *handle_{};
    PrismAppModuleV1 api_{};
    std::uint64_t load_duration_ns_{};
};
} // namespace prism::launch
