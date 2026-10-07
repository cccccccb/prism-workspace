#include "metrics.hpp"
#include "prism/app/module_support.hpp"
#include <iomanip>
#include <sstream>
#include <string>

namespace {
constexpr std::uint32_t kResetMonitoringChoice = 1;

std::string_view View(PrismStringViewV1 value)
{
    return value.data ? std::string_view(value.data, value.size) : std::string_view{};
}

std::string Fixed(double value, unsigned precision = 0)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(precision) << value;
    return text.str();
}

struct Settings {
    const PrismHostApiV1 *host;
    prism::settings::LinuxMetrics sampler{};
    std::uint64_t theme_request{}, theme_generation{}, interval_ns{1000000000ULL};
    std::uint64_t reset_request{};
    bool monitoring{true};

    bool Selection(std::string_view id)
    {
        return prism::app::Boolean(host, "theme_glass_selected", id == "glass") &&
               prism::app::Boolean(host, "theme_translucent_selected", id == "translucent") &&
               prism::app::Boolean(host, "theme_transparent_selected", id == "transparent") &&
               prism::app::Boolean(host, "theme_square_selected", id == "square");
    }

    bool SchemeSelection(std::string_view scheme)
    {
        return prism::app::Boolean(host, "scheme_light_selected", scheme == "light") &&
               prism::app::Boolean(host, "scheme_dark_selected", scheme == "dark");
    }

    bool Page(int page)
    {
        return prism::app::Boolean(host, "page_performance", page == 0) &&
               prism::app::Boolean(host, "page_appearance", page == 1) &&
               prism::app::Boolean(host, "page_system", page == 3);
    }

    bool Error(std::string_view detail = {})
    {
        return prism::app::Text(host, "theme_status", detail) &&
               prism::app::Boolean(host, "theme_error_visible", !detail.empty()) &&
               prism::app::Boolean(host, "theme_normal", detail.empty());
    }

    bool Interval(std::uint64_t interval)
    {
        interval_ns = interval;
        return prism::app::Boolean(host, "interval_500_selected", interval == 500000000ULL) &&
               prism::app::Boolean(host, "interval_1000_selected", interval == 1000000000ULL) &&
               prism::app::Boolean(host, "interval_2000_selected", interval == 2000000000ULL);
    }

    bool Schedule()
    {
        return !monitoring || prism::app::Tick(host, interval_ns);
    }

    bool RequestMonitoringReset()
    {
        if (reset_request) {
            return true;
        }
        if (host->struct_size < offsetof(PrismHostApiV1, cancel_task) + sizeof(host->cancel_task) ||
            !host->task_capabilities || !host->request_task || !host->cancel_task ||
            !(host->task_capabilities(host->context) & PRISM_TASK_CAP_CONFIRMATION_V1)) {
            return Error("Confirmation is unavailable in this session");
        }

        const PrismTaskChoiceV1 choice{sizeof(choice),
                                       kResetMonitoringChoice,
                                       {"Reset", sizeof("Reset") - 1},
                                       PRISM_TASK_CHOICE_PRIMARY_V1};
        const PrismTaskRequestV1 request{
            sizeof(request),
            PRISM_TASK_CONFIRMATION_V1,
            {"Reset monitoring?", sizeof("Reset monitoring?") - 1},
            {"Turn monitoring on and restore a 1-second sampling interval.",
             sizeof("Turn monitoring on and restore a 1-second sampling interval.") - 1},
            &choice,
            1,
            nullptr};
        reset_request = host->request_task(host->context, &request);
        if (!reset_request) {
            return Error("Unable to open confirmation. Please try again");
        }
        return true;
    }

    bool ResetMonitoring()
    {
        monitoring = true;
        sampler.ResetCpu();

        if (!prism::app::Boolean(host, "monitoring_active", true) || !Interval(1000000000ULL) ||
            !Metrics() || !Schedule()) {
            return Error("Unable to restore monitoring. Please try again");
        }
        return Error();
    }

    bool Metrics()
    {
        const auto sample = sampler.Sample();
        const auto cpu = sample.cpu_usage ? Fixed(*sample.cpu_usage * 100) + "%" : "—";

        std::string memory = "—";
        std::string gpu = "—";
        double memory_usage{};
        if (sample.memory) {
            memory_usage = static_cast<double>(sample.memory->used_kib) / sample.memory->total_kib;
            memory = Fixed(sample.memory->used_kib / 1048576.0, 1) + " / " +
                     Fixed(sample.memory->total_kib / 1048576.0, 1) + " GiB";
        }

        if (sample.gpu_busy) {
            gpu = Fixed(*sample.gpu_busy * 100) + "%";
        }
        if (sample.gpu_frequency_hz) {
            if (gpu == "—") {
                gpu.clear();
            } else {
                gpu += " · ";
            }
            gpu += Fixed(*sample.gpu_frequency_hz / 1000000.0) + " MHz";
        }
        if (sample.gpu_temperature_c) {
            if (gpu == "—") {
                gpu.clear();
            } else {
                gpu += " · ";
            }
            gpu += Fixed(*sample.gpu_temperature_c) + "°C";
        }

        return prism::app::Number(host, "cpu_usage", sample.cpu_usage.value_or(0)) &&
               prism::app::Number(host, "memory_usage", memory_usage) &&
               prism::app::Text(host, "cpu_usage_text", cpu) &&
               prism::app::Text(host, "mem_usage_text", memory) &&
               prism::app::Text(host, "gpu_usage_text", gpu);
    }
};

void *Create(const PrismAppInitV1 *init) noexcept
{
    if (!prism::app::ValidHost(init)) {
        return nullptr;
    }

    Settings *settings = nullptr;
    try {
        settings = new Settings{init->host};
        if (settings->Selection({}) && settings->SchemeSelection({}) && settings->Page(0) &&
            settings->Error() && settings->Interval(settings->interval_ns) &&
            prism::app::Boolean(init->host, "monitoring_active", true) && settings->Metrics() &&
            settings->Schedule()) {
            settings->theme_request = prism::app::SelectTheme(init->host, {});
            if (!settings->theme_request) {
                settings->Error("Theme unavailable");
            }
            if (prism::app::Ready(init->host)) {
                return settings;
            }
        }
    } catch (...) {
    }

    delete settings;
    return nullptr;
}

void Destroy(void *instance) noexcept
{
    delete static_cast<Settings *>(instance);
}

void Action(void *instance, PrismStringViewV1 value) noexcept
{
    try {
        auto &settings = *static_cast<Settings *>(instance);
        const auto action = View(value);

        if (action == "error:dismiss") {
            settings.Error();
        } else if (action == "sys:refresh") {
            settings.Metrics(); // A manual sample does not move the periodic deadline.
        } else if (action == "monitor:reset") {
            settings.RequestMonitoringReset();
        } else if (action == "page:performance") {
            settings.Page(0);
        } else if (action == "page:appearance") {
            settings.Page(1);
        } else if (action == "page:system") {
            settings.Page(3);
        } else if (action == "monitor:toggle") {
            settings.monitoring = !settings.monitoring;
            prism::app::Boolean(settings.host, "monitoring_active", settings.monitoring);
            settings.sampler.ResetCpu();
            if (settings.monitoring) {
                settings.Metrics();
                settings.Schedule();
            }
            // One previously scheduled callback may arrive while paused; Tick
            // consumes it without file/ioctl reads or scheduling another tick.
        } else if (action == "sampling:500" || action == "sampling:1000" ||
                   action == "sampling:2000") {
            const auto interval = action == "sampling:500"    ? 500000000ULL
                                  : action == "sampling:1000" ? 1000000000ULL
                                                              : 2000000000ULL;
            settings.Interval(interval);
            settings.Schedule();
        } else if (action == "theme:glass" || action == "theme:translucent" ||
                   action == "theme:transparent" || action == "theme:square") {
            settings.Error();
            settings.theme_request = prism::app::SelectTheme(settings.host, action.substr(6));
            if (!settings.theme_request) {
                settings.Error("Theme unavailable");
            }
        } else if (action == "appearance:light" || action == "appearance:dark") {
            settings.Error();
            settings.theme_request =
                prism::app::SelectColorScheme(settings.host, action.substr(11));
            if (!settings.theme_request) {
                settings.Error("Color scheme unavailable");
            }
        }
    } catch (...) {
    }
}

void Tick(void *instance, std::uint64_t) noexcept
{
    try {
        auto &settings = *static_cast<Settings *>(instance);
        if (settings.monitoring) {
            settings.Metrics();
            settings.Schedule();
        }
    } catch (...) {
    }
}

void ThemeEvent(void *instance, const PrismThemeEventV1 *event) noexcept
{
    try {
        constexpr auto base_size = offsetof(PrismThemeEventV1, color_scheme);
        if (!event || event->struct_size < base_size) {
            return;
        }

        auto &settings = *static_cast<Settings *>(instance);
        if (event->status == 2) {
            if (event->request_id == settings.theme_request) {
                settings.Error(View(event->detail).empty() ? "Appearance change rejected"
                                                           : View(event->detail));
            }
            return;
        }
        if (event->status > 1 || event->generation < settings.theme_generation) {
            return;
        }

        settings.theme_generation = event->generation;
        settings.Selection(View(event->id));
        const auto scheme = event->struct_size >= base_size + sizeof(event->color_scheme)
                                ? View(event->color_scheme)
                                : "dark";
        // Selection is confirmed by the platform event, never guessed on click.
        settings.SchemeSelection(scheme);
        settings.Error();
    } catch (...) {
    }
}

void TaskCompleted(void *instance, const PrismTaskResultV1 *result) noexcept
{
    try {
        constexpr auto confirmation_size =
            offsetof(PrismTaskResultV1, diagnostic) + sizeof(PrismTaskResultV1::diagnostic);
        if (!instance || !result || result->struct_size < confirmation_size) {
            return;
        }
        auto &settings = *static_cast<Settings *>(instance);
        if (!settings.reset_request || result->request_id != settings.reset_request ||
            result->outcome > PRISM_TASK_FAILED_V1 ||
            (result->outcome == PRISM_TASK_SUCCEEDED_V1 &&
             result->choice_id != kResetMonitoringChoice)) {
            return;
        }

        settings.reset_request = 0;
        if (result->outcome == PRISM_TASK_SUCCEEDED_V1) {
            settings.ResetMonitoring();
        } else if (result->outcome == PRISM_TASK_FAILED_V1) {
            settings.Error("Monitoring reset could not open. Please try again");
        }
        // Cancel keeps monitoring and its existing one-shot sampling schedule.
    } catch (...) {
    }
}

const PrismAppModuleV1 api{sizeof(api), PRISM_APP_ABI_V1, Create,  Destroy,    Action,
                           Tick,        nullptr,          nullptr, ThemeEvent, nullptr,
                           nullptr,     nullptr,          nullptr, nullptr,    nullptr,
                           nullptr,     TaskCompleted};
} // namespace

extern "C" PRISM_APP_EXPORT const PrismAppModuleV1 *prism_app_module_v1()
{
    return &api;
}
