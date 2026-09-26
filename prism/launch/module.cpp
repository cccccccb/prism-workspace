#include "prism/launch/module.hpp"
#include "prism/launch/error.hpp"
#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <cstring>
#include <algorithm>

namespace prism::launch {
AppModule::AppModule(const std::filesystem::path& file) {
    handle_ = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) throw LaunchFailure(contracts::LaunchError::ModuleLoadFailed, std::string("Cannot load application module: ") + dlerror());
    try {
        dlerror();
        auto entry = reinterpret_cast<PrismAppEntryV1>(dlsym(handle_, PRISM_APP_ENTRY_V1));
        if (dlerror() || !entry) throw LaunchFailure(contracts::LaunchError::ModuleLoadFailed, "Application module entry v1 missing");
        const auto* source=entry();
        if (!source || source->struct_size<offsetof(PrismAppModuleV1,on_action) ||
            source->abi_version!=PRISM_APP_ABI_V1)
            throw LaunchFailure(contracts::LaunchError::UnsupportedAbi,"Incompatible application module ABI v1");
        // Copy complete fields only; optional missing callbacks stay null.
        api_.struct_size=sizeof(api_); api_.abi_version=source->abi_version;
#define COPY_FIELD(field) if (source->struct_size>=offsetof(PrismAppModuleV1,field)+sizeof(source->field)) api_.field=source->field
        COPY_FIELD(create); COPY_FIELD(destroy); COPY_FIELD(on_action); COPY_FIELD(on_tick);
        COPY_FIELD(on_launch_event); COPY_FIELD(on_instance_event); COPY_FIELD(on_theme_event);
#undef COPY_FIELD
        if (!api_.create || !api_.destroy)
            throw LaunchFailure(contracts::LaunchError::UnsupportedAbi,"Module create/destroy missing");
    } catch (...) {
        dlclose(handle_);
        handle_ = nullptr;
        throw;
    }
}
AppModule::~AppModule() { if (handle_) dlclose(handle_); }
} // namespace prism::launch
