#pragma once
#include <cstdint>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <string_view>

namespace prism::settings {
struct CpuCounters {
    std::uint64_t total{}, idle{};
};

struct Memory {
    std::uint64_t used_kib{}, total_kib{};
};

std::optional<CpuCounters> ReadCpu(std::istream &);
std::optional<Memory> ReadMemory(std::istream &);
std::optional<std::uint64_t> ParseGpuClock(std::string_view);
std::optional<std::uint32_t> ParseThrottled(std::string_view);

class CpuTracker {
public:
    std::optional<double> Update(std::optional<CpuCounters>);

    void Reset()
    {
        previous_.reset();
    }

private:
    std::optional<CpuCounters> previous_;
};

struct Metrics {
    std::optional<double> cpu_usage;
    std::optional<Memory> memory;
    // Busy is present only when a driver publishes an actual utilization value.
    std::optional<double> gpu_busy;
    std::optional<std::uint64_t> gpu_frequency_hz;
    std::optional<double> gpu_temperature_c, soc_temperature_c;
    std::optional<std::uint32_t> throttled;
};

struct MetricPaths {
    std::filesystem::path proc{"/proc"}, sys{"/sys"};
    std::filesystem::path firmware{"/dev/vcio_gencmd"}, firmware_fallback{"/dev/vcio"};
};

// Linux/Pi read-only business telemetry; no renderer, host or process dependency.
class LinuxMetrics {
public:
    explicit LinuxMetrics(MetricPaths paths = {});
    ~LinuxMetrics();
    LinuxMetrics(const LinuxMetrics &) = delete;
    LinuxMetrics &operator=(const LinuxMetrics &) = delete;
    Metrics Sample();

    void ResetCpu()
    {
        cpu_.Reset();
    }

private:
    MetricPaths paths_;
    CpuTracker cpu_;
    int firmware_fd_{-1};
    std::filesystem::path gpu_frequency_, gpu_busy_, gpu_temperature_, soc_temperature_;
    std::optional<std::string> FirmwareClock();
    std::optional<std::string> FirmwareThrottled();
    std::optional<std::string> FirmwareRead(bool clock);
};
} // namespace prism::settings
