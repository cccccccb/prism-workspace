#include "prism/app/module_support.hpp"
#include "metrics.hpp"
#include <iomanip>
#include <sstream>
#include <string>

namespace {
std::string_view View(PrismStringViewV1 value) {
    return value.data?std::string_view(value.data,value.size):std::string_view{};
}
std::string Fixed(double value,unsigned precision=0) {
    std::ostringstream text; text<<std::fixed<<std::setprecision(precision)<<value; return text.str();
}
struct Settings {
    const PrismHostApiV1* host;
    prism::settings::LinuxMetrics sampler{};
    std::uint64_t theme_request{},theme_generation{},interval_ns{1000000000ULL};
    bool monitoring{true};

    bool Selection(std::string_view id) {
        return prism::app::Boolean(host,"theme_glass_selected",id=="glass") &&
            prism::app::Boolean(host,"theme_translucent_selected",id=="translucent") &&
            prism::app::Boolean(host,"theme_transparent_selected",id=="transparent") &&
            prism::app::Boolean(host,"theme_square_selected",id=="square");
    }
    bool SchemeSelection(std::string_view scheme) {
        return prism::app::Boolean(host,"scheme_light_selected",scheme=="light") &&
            prism::app::Boolean(host,"scheme_dark_selected",scheme=="dark");
    }
    bool Page(bool performance) {
        return prism::app::Boolean(host,"page_performance",performance) &&
            prism::app::Boolean(host,"page_appearance",!performance);
    }
    bool Error(std::string_view detail={}) {
        return prism::app::Text(host,"theme_status",detail) &&
            prism::app::Boolean(host,"theme_error_visible",!detail.empty());
    }
    bool Interval(std::uint64_t interval) {
        interval_ns=interval;
        return prism::app::Boolean(host,"interval_500_selected",interval==500000000ULL) &&
            prism::app::Boolean(host,"interval_1000_selected",interval==1000000000ULL) &&
            prism::app::Boolean(host,"interval_2000_selected",interval==2000000000ULL);
    }
    bool Schedule() { return !monitoring || prism::app::Tick(host,interval_ns); }
    bool Metrics() {
        const auto sample=sampler.Sample();
        const auto cpu=sample.cpu_usage?Fixed(*sample.cpu_usage*100)+"%":"—";
        std::string memory="—",gpu="—",aux;
        double memory_usage{};
        if (sample.memory) {
            memory=Fixed(sample.memory->used_kib/1048576.0,1)+" / "+
                Fixed(sample.memory->total_kib/1048576.0,1)+" GiB";
            memory_usage=static_cast<double>(sample.memory->used_kib)/sample.memory->total_kib;
        }
        if (sample.gpu_busy) gpu=Fixed(*sample.gpu_busy*100)+"%";
        if (sample.gpu_frequency_hz) {
            if (gpu=="—") gpu.clear(); else gpu+=" · ";
            gpu+=Fixed(*sample.gpu_frequency_hz/1000000.0)+" MHz";
        }
        if (sample.gpu_temperature_c) {
            if (gpu=="—") gpu.clear(); else gpu+=" · ";
            gpu+=Fixed(*sample.gpu_temperature_c)+"°C";
        }
        if (sample.soc_temperature_c) aux="SoC "+Fixed(*sample.soc_temperature_c)+"°C";
        if (sample.throttled) {
            if (!aux.empty()) aux+=" · ";
            // Low bits describe current limits; high bits record past events.
            aux+=(*sample.throttled&0xf)?"limits active":
                (*sample.throttled&0xf0000)?"limits history":"limits clear";
        }
        return prism::app::Text(host,"cpu_usage_text",cpu) &&
            prism::app::Text(host,"mem_usage_text",memory) &&
            prism::app::Number(host,"cpu_usage",sample.cpu_usage.value_or(0)) &&
            prism::app::Number(host,"memory_usage",memory_usage) &&
            prism::app::Text(host,"gpu_usage_text",gpu) &&
            prism::app::Number(host,"gpu_busy",sample.gpu_busy.value_or(0)) &&
            prism::app::Boolean(host,"gpu_busy_available",sample.gpu_busy.has_value()) &&
            prism::app::Text(host,"hardware_aux",aux) &&
            prism::app::Boolean(host,"hardware_aux_visible",!aux.empty());
    }
};
void* Create(const PrismAppInitV1* init) noexcept {
    if (!prism::app::ValidHost(init)) return nullptr;
    Settings* settings=nullptr;
    try {
        settings=new Settings{init->host};
        if (settings->Selection({}) && settings->SchemeSelection({}) && settings->Page(true) &&
            settings->Error() && settings->Interval(settings->interval_ns) &&
            prism::app::Boolean(init->host,"monitoring_active",true) && settings->Metrics() && settings->Schedule()) {
            settings->theme_request=prism::app::SelectTheme(init->host,{});
            if (!settings->theme_request) settings->Error("Theme unavailable");
            if (prism::app::Ready(init->host)) return settings;
        }
    } catch (...) {} delete settings; return nullptr;
}
void Destroy(void* instance) noexcept { delete static_cast<Settings*>(instance); }
void Action(void* instance,PrismStringViewV1 value) noexcept {
    try {
        auto& settings=*static_cast<Settings*>(instance); const auto action=View(value);
        if (action=="sys:refresh") settings.Metrics(); // A manual sample does not move the periodic deadline.
        else if (action=="page:performance" || action=="page:appearance") settings.Page(action=="page:performance");
        else if (action=="monitor:toggle") {
            settings.monitoring=!settings.monitoring;
            prism::app::Boolean(settings.host,"monitoring_active",settings.monitoring);
            settings.sampler.ResetCpu();
            if (settings.monitoring) { settings.Metrics(); settings.Schedule(); }
            // One previously scheduled callback may arrive while paused; Tick
            // consumes it without file/ioctl reads or scheduling another tick.
        } else if (action=="sampling:500" || action=="sampling:1000" || action=="sampling:2000") {
            const auto interval=action=="sampling:500"?500000000ULL:
                action=="sampling:1000"?1000000000ULL:2000000000ULL;
            settings.Interval(interval); settings.Schedule();
        } else if (action=="theme:glass" || action=="theme:translucent" ||
                   action=="theme:transparent" || action=="theme:square") {
            settings.Error();
            settings.theme_request=prism::app::SelectTheme(settings.host,action.substr(6));
            if (!settings.theme_request) settings.Error("Theme unavailable");
        } else if (action=="appearance:light" || action=="appearance:dark") {
            settings.Error();
            settings.theme_request=prism::app::SelectColorScheme(settings.host,action.substr(11));
            if (!settings.theme_request) settings.Error("Color scheme unavailable");
        }
    } catch (...) {}
}
void Tick(void* instance,std::uint64_t) noexcept {
    try {
        auto& settings=*static_cast<Settings*>(instance);
        if (settings.monitoring) { settings.Metrics(); settings.Schedule(); }
    } catch (...) {}
}
void ThemeEvent(void* instance,const PrismThemeEventV1* event) noexcept {
    try {
        constexpr auto base_size=offsetof(PrismThemeEventV1,color_scheme);
        if (!event || event->struct_size<base_size) return;
        auto& settings=*static_cast<Settings*>(instance);
        if (event->status==2) {
            if (event->request_id==settings.theme_request)
                settings.Error(View(event->detail).empty()?"Appearance change rejected":View(event->detail));
            return;
        }
        if (event->status>1 || event->generation<settings.theme_generation) return;
        settings.theme_generation=event->generation;
        settings.Selection(View(event->id));
        const auto scheme=event->struct_size>=base_size+sizeof(event->color_scheme)?View(event->color_scheme):"dark";
        settings.SchemeSelection(scheme); // Selection is confirmed by the platform event, never guessed on click.
        settings.Error();
    } catch (...) {}
}
const PrismAppModuleV1 api{sizeof(api),PRISM_APP_ABI_V1,Create,Destroy,Action,Tick,nullptr,nullptr,ThemeEvent};
}
extern "C" PRISM_APP_EXPORT const PrismAppModuleV1* prism_app_module_v1() { return &api; }
