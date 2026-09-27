#include "prism/app/module_support.hpp"
#include "prism/contracts/app_module.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace {
static_assert(PRISM_APP_ABI_V1 == 1u);

class Counter {
public:
    explicit Counter(const PrismHostApiV1 *host) : host_(host)
    {
    }

    bool Publish()
    {
        const auto label = std::to_string(count_);
        const auto status = "Value " + label + " of " + std::to_string(Limit);
        return prism::app::Text(host_, "count_text", label) &&
               prism::app::Number(host_, "count_progress", static_cast<double>(count_) / Limit) &&
               prism::app::Text(host_, "status", status) &&
               prism::app::Boolean(host_, "detail_visible", detail_visible_);
    }

    bool Action(std::string_view action)
    {
        if (action == "counter:increment") {
            count_ = std::min(count_ + 1, Limit);
        } else if (action == "counter:decrement") {
            count_ = std::max(count_ - 1, 0);
        } else if (action == "counter:reset") {
            count_ = 0;
        } else if (action == "counter:details") {
            detail_visible_ = !detail_visible_;
        } else {
            return true;
        }
        return Publish();
    }

private:
    static constexpr int Limit = 10;
    const PrismHostApiV1 *host_;
    int count_{};
    bool detail_visible_{true};
};

bool HasRequiredHost(const PrismAppInitV1 *init)
{
    if (!prism::app::ValidHost(init)) {
        return false;
    }
    // Only base binding/Ready capabilities are used. No optional work, theme,
    // subscription or assets-root tail is required by this in-memory example.
    const auto *host = init->host;
    return host->struct_size >=
               offsetof(PrismHostApiV1, backend_ready) + sizeof(host->backend_ready) &&
           host->set_binding && host->backend_ready;
}

void *Create(const PrismAppInitV1 *init) noexcept
{
    if (!HasRequiredHost(init)) {
        return nullptr;
    }
    try {
        auto counter = std::make_unique<Counter>(init->host);
        if (!counter->Publish() || !prism::app::Ready(init->host)) {
            return nullptr;
        }
        return counter.release();
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void *instance) noexcept
{
    delete static_cast<Counter *>(instance);
}

void Action(void *instance, PrismStringViewV1 action) noexcept
{
    if (!instance || !action.data || action.size > 128) {
        return;
    }
    try {
        auto &counter = *static_cast<Counter *>(instance);
        if (!counter.Action(std::string_view(action.data, action.size))) {
            std::fprintf(stderr, "[counter_app] Host rejected a declared binding update\n");
        }
    } catch (...) {
        std::fprintf(stderr, "[counter_app] Action failed\n");
    }
}

const PrismAppModuleV1 module{sizeof(module), PRISM_APP_ABI_V1, Create,  Destroy, Action,
                              nullptr,        nullptr,          nullptr, nullptr, nullptr};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &module;
}
