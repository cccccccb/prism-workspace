#include "metrics.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace prism::settings {
namespace {
std::vector<std::filesystem::path> Children(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    for (std::filesystem::directory_iterator it(path,error), end; !error && it!=end; it.increment(error))
        result.push_back(it->path());
    std::sort(result.begin(),result.end());
    return result;
}
std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string value; std::getline(file,value); return value;
}
template<class T> std::optional<T> Unsigned(std::string_view value,int base=10) {
    T result{};
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result,base);
    if (value.empty() || parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) return {};
    return result;
}
std::optional<double> ReadNumber(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::istringstream input(ReadText(path)); double value{};
    if (!(input>>value) || !std::isfinite(value)) return {};
    input>>std::ws;
    if (!input.eof()) return {};
    return value;
}
bool File(const std::filesystem::path& path) {
    std::error_code error; return std::filesystem::is_regular_file(path,error);
}
bool GpuName(std::string_view name) {
    return name.find("v3d")!=name.npos || name.find("gpu")!=name.npos ||
        name.find("mali")!=name.npos || name.find("panfrost")!=name.npos ||
        name.find("lima")!=name.npos;
}
std::optional<double> Temperature(const std::filesystem::path& path) {
    const auto value=ReadNumber(path);
    if (!value || *value < -40000 || *value>150000) return {};
    return *value/1000;
}
} // namespace

std::optional<CpuCounters> ReadCpu(std::istream& source) {
    std::string line,label; std::getline(source,line);
    std::istringstream row(line); row>>label;
    if (label!="cpu") return {};
    CpuCounters value{}; std::string token; unsigned count{};
    // guest/guest_nice are already included in user/nice: do not sum twice.
    for (;count<8 && row>>token;++count) {
        const auto part=Unsigned<std::uint64_t>(token);
        if (!part || *part>std::numeric_limits<std::uint64_t>::max()-value.total) return {};
        value.total+=*part;
        if (count==3 || count==4) value.idle+=*part;
    }
    if (count<4 || (!row.eof() && row.fail())) return {};
    return value;
}
std::optional<Memory> ReadMemory(std::istream& source) {
    std::string line,label,unit;
    std::optional<std::uint64_t> total,available;
    while (std::getline(source,line)) {
        std::istringstream row(line); std::string token; row>>label;
        if (label!="MemTotal:" && label!="MemAvailable:") continue;
        if (!(row>>token>>unit) || unit!="kB") return {};
        const auto value=Unsigned<std::uint64_t>(token);
        if (!value) return {};
        (label=="MemTotal:"?total:available)=*value;
    }
    if (!total || !*total || !available || *available>*total) return {};
    return Memory{*total-*available,*total};
}
std::optional<double> CpuTracker::Update(std::optional<CpuCounters> value) {
    const auto previous=previous_; previous_=value;
    if (!value || !previous || value->total<=previous->total || value->idle<previous->idle) return {};
    const auto elapsed=value->total-previous->total, idle=value->idle-previous->idle;
    if (idle>elapsed) return {};
    return static_cast<double>(elapsed-idle)/elapsed;
}
std::optional<std::uint64_t> ParseGpuClock(std::string_view value) {
    constexpr std::string_view prefix="frequency(";
    if (!value.starts_with(prefix)) return {};
    const auto separator=value.find(")=",prefix.size());
    if (separator==value.npos || !Unsigned<std::uint32_t>(value.substr(prefix.size(),separator-prefix.size()))) return {};
    const auto clock=Unsigned<std::uint64_t>(value.substr(separator+2));
    if (!clock || !*clock) return {};
    return clock;
}
std::optional<std::uint32_t> ParseThrottled(std::string_view value) {
    constexpr std::string_view prefix="throttled=0x";
    if (!value.starts_with(prefix)) return {};
    return Unsigned<std::uint32_t>(value.substr(prefix.size()),16);
}

LinuxMetrics::LinuxMetrics(MetricPaths paths):paths_(std::move(paths)) {
    for (const auto& device:Children(paths_.sys/"class/devfreq")) {
        std::error_code error;
        const auto resolved=std::filesystem::weakly_canonical(device,error);
        if (!GpuName(device.filename().string()) && !GpuName(resolved.filename().string())) continue;
        if (gpu_frequency_.empty() && File(device/"cur_freq")) gpu_frequency_=device/"cur_freq";
        if (gpu_busy_.empty() && File(device/"device/gpu_busy_percent")) gpu_busy_=device/"device/gpu_busy_percent";
    }
    for (const auto& render:Children(paths_.sys/"class/drm")) {
        if (!render.filename().string().starts_with("renderD")) continue;
        const auto device=render/"device";
        if (gpu_busy_.empty() && File(device/"gpu_busy_percent")) gpu_busy_=device/"gpu_busy_percent";
        for (const auto& hwmon:Children(device/"hwmon")) {
            const auto name=ReadText(hwmon/"name");
            if (name.find("cpu")!=name.npos || name.find("soc")!=name.npos) continue;
            if (gpu_temperature_.empty() && File(hwmon/"temp1_input")) gpu_temperature_=hwmon/"temp1_input";
        }
    }
    for (const auto& thermal:Children(paths_.sys/"class/thermal")) {
        if (!thermal.filename().string().starts_with("thermal_zone")) continue;
        const auto type=ReadText(thermal/"type");
        if ((type.find("cpu")!=type.npos || type.find("soc")!=type.npos) && File(thermal/"temp")) {
            soc_temperature_=thermal/"temp"; break;
        }
    }
    // Open last so a discovery/allocation exception cannot leak the fd.
    // Missing firmware or permissions are ordinary capability absence.
    if (!paths_.firmware.empty()) firmware_fd_=open(paths_.firmware.c_str(),O_RDONLY|O_CLOEXEC);
    if (firmware_fd_<0 && !paths_.firmware_fallback.empty())
        firmware_fd_=open(paths_.firmware_fallback.c_str(),O_RDONLY|O_CLOEXEC);
}
LinuxMetrics::~LinuxMetrics() { if (firmware_fd_>=0) close(firmware_fd_); }
std::optional<std::string> LinuxMetrics::FirmwareClock() { return FirmwareRead(true); }
std::optional<std::string> LinuxMetrics::FirmwareThrottled() { return FirmwareRead(false); }
std::optional<std::string> LinuxMetrics::FirmwareRead(bool clock) {
    if (firmware_fd_<0) return {};
    // Raspberry Pi GET_GENCMD_RESULT property protocol. These two commands are
    // fixed, read-only telemetry; callers cannot submit firmware commands.
    constexpr std::uint32_t tag=0x00030080, capacity=4096, response=0x80000000;
    alignas(16) std::array<std::uint32_t,capacity/4+7> message{};
    message[0]=sizeof(message); message[2]=tag; message[3]=capacity;
    const char* command=clock?"measure_clock v3d":"get_throttled";
    std::memcpy(message.data()+6,command,std::strlen(command)+1);
    if (ioctl(firmware_fd_,_IOWR(100,0,char*),message.data())<0) return {};
    const auto length=message[4]&~response;
    if (message[0]!=sizeof(message) || message[1]!=response || message[2]!=tag ||
        message[3]!=capacity || !(message[4]&response) || length<=4 || length>capacity || message[5]!=0)
        return {};
    const auto* text=reinterpret_cast<const char*>(message.data()+6);
    const auto* end=static_cast<const char*>(std::memchr(text,0,length-4));
    if (!end) return {};
    return std::string(text,end);
}
Metrics LinuxMetrics::Sample() {
    Metrics result;
    std::ifstream stat(paths_.proc/"stat"), memory(paths_.proc/"meminfo");
    result.cpu_usage=cpu_.Update(ReadCpu(stat)); result.memory=ReadMemory(memory);
    if (const auto frequency=ReadNumber(gpu_frequency_);frequency && *frequency>0 && *frequency<1e12)
        result.gpu_frequency_hz=static_cast<std::uint64_t>(*frequency);
    if (!result.gpu_frequency_hz) if (const auto clock=FirmwareClock()) result.gpu_frequency_hz=ParseGpuClock(*clock);
    if (const auto busy=ReadNumber(gpu_busy_);busy && *busy>=0 && *busy<=100) result.gpu_busy=*busy/100;
    result.gpu_temperature_c=Temperature(gpu_temperature_);
    result.soc_temperature_c=Temperature(soc_temperature_);
    if (const auto throttled=FirmwareThrottled()) result.throttled=ParseThrottled(*throttled);
    return result;
}
} // namespace prism::settings
