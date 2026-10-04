#include "prism/launch/module.hpp"
#include "prism/launch/error.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <string>

namespace prism::launch {
AppModule::AppModule(const std::filesystem::path &file)
{
    const auto started = std::chrono::steady_clock::now();
    handle_ = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        throw LaunchFailure(contracts::LaunchError::ModuleLoadFailed,
                            std::string("Cannot load application module: ") + dlerror());
    }
    try {
        dlerror();
        auto entry = reinterpret_cast<PrismAppEntryV1>(dlsym(handle_, PRISM_APP_ENTRY_V1));
        if (dlerror() || !entry) {
            throw LaunchFailure(contracts::LaunchError::ModuleLoadFailed,
                                "Application module entry v1 missing");
        }
        const auto *source = entry();
        if (!source || source->struct_size < offsetof(PrismAppModuleV1, on_action) ||
            source->abi_version != PRISM_APP_ABI_V1) {
            throw LaunchFailure(contracts::LaunchError::UnsupportedAbi,
                                "Incompatible application module ABI v1");
        }
        // Copy complete fields only; optional missing callbacks stay null.
        api_.struct_size = sizeof(api_);
        api_.abi_version = source->abi_version;
#define COPY_FIELD(field)                                                                          \
    if (source->struct_size >= offsetof(PrismAppModuleV1, field) + sizeof(source->field))          \
    api_.field = source->field
        COPY_FIELD(create);
        COPY_FIELD(destroy);
        COPY_FIELD(on_action);
        COPY_FIELD(on_tick);
        COPY_FIELD(on_launch_event);
        COPY_FIELD(on_instance_event);
        COPY_FIELD(on_theme_event);
        COPY_FIELD(on_work_completed);
        COPY_FIELD(on_gesture);
        COPY_FIELD(on_layout_state);
        COPY_FIELD(on_layout_control_result);
        COPY_FIELD(on_text_edit);
        COPY_FIELD(on_close_requested);
#undef COPY_FIELD
        if (!api_.create || !api_.destroy) {
            throw LaunchFailure(contracts::LaunchError::UnsupportedAbi,
                                "Module create/destroy missing");
        }
        load_duration_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    } catch (...) {
        dlclose(handle_);
        handle_ = nullptr;
        throw;
    }
}

AppModule::~AppModule()
{
    if (handle_) {
        dlclose(handle_);
    }
}
} // namespace prism::launch
