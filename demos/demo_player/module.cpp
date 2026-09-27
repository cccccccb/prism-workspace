#include "player.hpp"
#include "prism/app/module_support.hpp"
#include <cstddef>
#include <string_view>

namespace {
using prism::music::Player;

void *Create(const PrismAppInitV1 *init) noexcept
{
    if (!prism::app::ValidHost(init) ||
        init->struct_size < offsetof(PrismAppInitV1, assets_root) + sizeof(init->assets_root) ||
        !init->assets_root.data || !init->assets_root.size || init->assets_root.size > 4096 ||
        init->assets_root.data[0] != '/' ||
        init->host->struct_size <
            offsetof(PrismHostApiV1, cancel_work) + sizeof(init->host->cancel_work) ||
        !init->host->submit_work || !init->host->cancel_work) {
        return nullptr;
    }

    Player *player = nullptr;
    try {
        const std::string_view assets(init->assets_root.data, init->assets_root.size);
        if (assets.find('\0') != std::string_view::npos) {
            return nullptr;
        }
        player = new Player(init->host, std::string(assets));
        if (!player->Initialize()) {
            delete player;
            return nullptr;
        }
        return player;
    } catch (...) {
        delete player;
        return nullptr;
    }
}

void Destroy(void *instance) noexcept
{
    delete static_cast<Player *>(instance);
}

void Action(void *instance, PrismStringViewV1 text) noexcept
{
    if (!text.data && text.size) {
        return;
    }
    try {
        static_cast<Player *>(instance)->Action(std::string_view(text.data, text.size));
    } catch (...) {
    }
}

void Tick(void *instance, std::uint64_t) noexcept
{
    try {
        static_cast<Player *>(instance)->Tick();
    } catch (...) {
    }
}

void Completed(void *instance, const PrismWorkCompletionV1 *completion) noexcept
{
    if (!completion || completion->struct_size < sizeof(*completion)) {
        return;
    }
    try {
        static_cast<Player *>(instance)->Completed(*completion);
    } catch (...) {
    }
}

const PrismAppModuleV1 api{sizeof(api), PRISM_APP_ABI_V1, Create,  Destroy, Action,
                           Tick,        nullptr,          nullptr, nullptr, Completed};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &api;
}
