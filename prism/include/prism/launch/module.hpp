#pragma once
#include "prism/contracts/app_module.h"
#include <filesystem>

namespace prism::launch {
// Loader lifetime must outlive every instance created through Api().
class AppModule {
public:
    explicit AppModule(const std::filesystem::path& file);
    ~AppModule();
    AppModule(const AppModule&) = delete;
    AppModule& operator=(const AppModule&) = delete;
    const PrismAppModuleV1& Api() const { return *api_; }
private:
    void* handle_{};
    const PrismAppModuleV1* api_{};
};
} // namespace prism::launch
