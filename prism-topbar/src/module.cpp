#include "prism/app/module_support.hpp"
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
namespace {
struct Topbar {
    const PrismHostApiV1* host;
    bool Update() {
        std::time_t now=std::time(nullptr); std::tm local{}; localtime_r(&now,&local);
        char clock[64]; std::strftime(clock,sizeof(clock),"%b-%d %H:%M:%S",&local);
        std::string network="Offline", battery="AC", power_icon="power";
        std::error_code error;
        for (const auto& item:std::filesystem::directory_iterator("/sys/class/net",error)) {
            if (item.path().filename()=="lo") continue;
            std::string state; std::ifstream(item.path()/"operstate")>>state;
            if (state=="up") { network=item.path().filename().string(); break; }
        }
        for (const auto& item:std::filesystem::directory_iterator("/sys/class/power_supply",error)) {
            std::string type; std::ifstream(item.path()/"type")>>type;
            if (type=="Battery") {
                power_icon="battery";
                int capacity{}; if (std::ifstream(item.path()/"capacity")>>capacity) battery=std::to_string(capacity)+"%";
                else battery="Battery N/A";
                break;
            }
        }
        return prism::app::Text(host,"clock_time",clock) && prism::app::Text(host,"net_status",network) &&
            prism::app::Text(host,"bat_status",battery) && prism::app::Text(host,"power_icon",power_icon) &&
            prism::app::Tick(host);
    }
};
void* Create(const PrismAppInitV1* init) noexcept {
    if (!prism::app::ValidHost(init)) return nullptr;
    Topbar* item=nullptr;
    try { item=new Topbar{init->host}; if (item->Update() && prism::app::Ready(init->host)) return item; }
    catch (...) {} delete item; return nullptr;
}
void Destroy(void* instance) noexcept { delete static_cast<Topbar*>(instance); }
void Tick(void* instance,std::uint64_t) noexcept { try { static_cast<Topbar*>(instance)->Update(); } catch (...) {} }
const PrismAppModuleV1 api{sizeof(api),PRISM_APP_ABI_V1,Create,Destroy,nullptr,Tick,nullptr,nullptr};
}
extern "C" PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1() { return &api; }
