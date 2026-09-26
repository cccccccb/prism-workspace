#include "prism/launch/module.hpp"
#include "prism/launch/error.hpp"
#include <dlfcn.h>
#include <stdexcept>
#include <string>

namespace prism::launch {
AppModule::AppModule(const std::filesystem::path& file) {
    handle_ = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) throw LaunchFailure(contracts::LaunchError::ModuleLoadFailed, std::string("Cannot load application module: ") + dlerror());
    try {
        dlerror();
        auto entry = reinterpret_cast<PrismAppEntryV1>(dlsym(handle_, PRISM_APP_ENTRY_V1));
        if (dlerror() || !entry) throw LaunchFailure(contracts::LaunchError::ModuleLoadFailed, "Application module entry v1 missing");
        api_ = entry();
        if (!api_ || api_->struct_size < sizeof(PrismAppModuleV1) ||
            api_->abi_version != PRISM_APP_ABI_V1 || !api_->create || !api_->destroy)
            throw LaunchFailure(contracts::LaunchError::UnsupportedAbi, "Incompatible application module ABI v1");
    } catch (...) {
        dlclose(handle_);
        handle_ = nullptr;
        throw;
    }
}
AppModule::~AppModule() { if (handle_) dlclose(handle_); }
} // namespace prism::launch
