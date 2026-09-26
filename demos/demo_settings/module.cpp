#include "prism/app/module_support.hpp"
#include "prism/contracts/theme_tokens.hpp"
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
namespace {
struct Settings {
    const PrismHostApiV1* host;
    bool dark{false};
    std::uint64_t previous_total{}, previous_idle{};
    bool Appearance() {
        namespace theme=prism::contracts::theme;
        return prism::app::Boolean(host,"theme_dark",dark) &&
            prism::app::Text(host,"dark_mode_btn",dark?"Appearance: Dark":"Appearance: Light") &&
            prism::app::Text(host,"appearance_icon",dark?"moon":"sun") &&
            prism::app::Color(host,"window_tint",dark?theme::kDarkWindowTint:theme::kWindowTint) &&
            prism::app::Color(host,"card_tint",dark?theme::kDarkCardTint:theme::kCardTint) &&
            prism::app::Color(host,"control_tint",dark?theme::kDarkControlTint:theme::kControlTint) &&
            prism::app::Color(host,"text_color",dark?theme::kDarkText:theme::kText) &&
            prism::app::Color(host,"muted_color",dark?theme::kDarkMutedText:theme::kMutedText);
    }
    bool Metrics() {
        std::ifstream stat("/proc/stat"); std::string label,line; std::getline(stat,line);
        std::istringstream cpu(line); cpu>>label; std::uint64_t part{},total{},idle{}; unsigned index=0;
        while (index<8 && cpu>>part) { total+=part; if (index==3 || index==4) idle+=part; ++index; }
        std::string cpu_text="CPU Load: sampling";
        double cpu_usage{};
        if (label=="cpu" && previous_total && total>previous_total && idle>=previous_idle) {
            auto elapsed=total-previous_total, sleeping=idle-previous_idle;
            cpu_usage=static_cast<double>(elapsed-std::min(elapsed,sleeping))/elapsed;
            cpu_text="CPU Load: "+std::to_string(static_cast<unsigned>(100.0*cpu_usage))+"%";
        }
        previous_total=total; previous_idle=idle;
        std::ifstream mem("/proc/meminfo"); std::uint64_t capacity{},available{};
        while (std::getline(mem,line)) {
            std::istringstream row(line); row>>label>>part;
            if (label=="MemTotal:") capacity=part;
            if (label=="MemAvailable:") available=part;
        }
        std::string memory="Memory: unavailable";
        if (capacity && available<=capacity) {
            std::ostringstream value; value<<std::fixed<<std::setprecision(2)<<"Memory: "
                <<(capacity-available)/1048576.0<<" / "<<capacity/1048576.0<<" GiB"; memory=value.str();
        }
        const auto memory_usage=capacity && available<=capacity
            ? static_cast<double>(capacity-available)/capacity:0;
        return prism::app::Text(host,"cpu_usage_text",cpu_text) && prism::app::Text(host,"mem_usage_text",memory) &&
            prism::app::Number(host,"cpu_usage",cpu_usage) && prism::app::Number(host,"memory_usage",memory_usage) &&
            prism::app::Tick(host);
    }
};
void* Create(const PrismAppInitV1* init) noexcept {
    if (!prism::app::ValidHost(init)) return nullptr;
    Settings* settings=nullptr;
    try {
        settings=new Settings{init->host};
        if (settings->Appearance() && settings->Metrics() && prism::app::Ready(init->host)) return settings;
    } catch (...) {} delete settings; return nullptr;
}
void Destroy(void* instance) noexcept { delete static_cast<Settings*>(instance); }
void Action(void* instance,PrismStringViewV1 value) noexcept {
    try {
        auto& settings=*static_cast<Settings*>(instance); std::string_view action(value.data,value.size);
        if (action=="theme:toggle") { settings.dark=!settings.dark; settings.Appearance(); }
        else if (action=="sys:refresh") settings.Metrics();
    } catch (...) {}
}
void Tick(void* instance,std::uint64_t) noexcept { try { static_cast<Settings*>(instance)->Metrics(); } catch (...) {} }
const PrismAppModuleV1 api{sizeof(api),PRISM_APP_ABI_V1,Create,Destroy,Action,Tick,nullptr,nullptr};
}
extern "C" PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1() { return &api; }
