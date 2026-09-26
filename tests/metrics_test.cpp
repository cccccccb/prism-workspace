#include "metrics.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
const std::filesystem::path fixtures=std::filesystem::path(PRISM_SOURCE_ROOT)/"tests/fixtures/metrics";
void Check(bool condition,const char* reason) { if (!condition) throw std::runtime_error(reason); }
std::optional<prism::settings::CpuCounters> Cpu(const char* name) {
    std::ifstream file(fixtures/name); return prism::settings::ReadCpu(file);
}
}
int main() {
    using namespace prism::settings;
    try {
        const auto first=Cpu("cpu_initial"), second=Cpu("cpu_next");
        Check(first && first->total==576 && first->idle==410,"CPU guest counters must not be double-counted");
        Check(second && second->total==692 && second->idle==465,"CPU counters and iowait must be read correctly");
        CpuTracker cpu;
        Check(!cpu.Update(first),"CPU usage needs two samples");
        const auto busy=cpu.Update(second);
        Check(busy && std::abs(*busy-61.0/116)<1e-12,"CPU usage must use elapsed deltas");
        Check(!cpu.Update(first),"CPU counter rollback must invalidate this interval");
        Check(cpu.Update(second).has_value(),"CPU tracker must recover after counter rollback");
        cpu.Reset(); Check(!cpu.Update(second),"Resume must not average a paused interval");
        std::istringstream negative("cpu -1 0 0 0"),overflow("cpu 18446744073709551615 1 0 0"),bad("cpu 1 x 2 3");
        Check(!ReadCpu(negative) && !ReadCpu(overflow) && !ReadCpu(bad),"Malformed CPU counters must be unavailable");
        std::ifstream valid_memory(fixtures/"available/proc/meminfo"),missing_memory(fixtures/"memory_missing");
        const auto memory=ReadMemory(valid_memory);
        Check(memory && memory->used_kib==6291456 && memory->total_kib==8388608,"Memory must use MemAvailable");
        Check(!ReadMemory(missing_memory),"Absent MemAvailable must not be presented as 100 percent usage");
        std::istringstream invalid_memory("MemTotal: 100 kB\nMemAvailable: 101 kB\n");
        Check(!ReadMemory(invalid_memory),"Inconsistent memory samples must be unavailable");
        Check(ParseGpuClock("frequency(46)=500000992")==500000992ULL,"Pi V3D domain 46 must report actual Hz");
        Check(!ParseGpuClock("frequency(46)=0") && !ParseGpuClock("frequency(46)=500 MHz") &&
            !ParseGpuClock("error=1"),"Invalid GPU responses must not become frequency or utilization");
        Check(ParseThrottled("throttled=0x50000")==0x50000U && ParseThrottled("throttled=0x0")==0U,
            "Firmware current/historical limit bit fields must be preserved");
        Check(!ParseThrottled("throttled=0xno") && !ParseThrottled("throttled=0x100000000"),
            "Malformed firmware limit responses must be rejected");
        LinuxMetrics available({fixtures/"available/proc",fixtures/"available/sys",{}, {}});
        const auto sample=available.Sample();
        Check(!sample.cpu_usage && sample.memory.has_value(),"Initial CPU is unavailable while memory remains usable");
        Check(sample.gpu_frequency_hz==600000000ULL && sample.gpu_busy && std::abs(*sample.gpu_busy-.375)<1e-12,
            "GPU frequency and driver utilization must remain distinct metrics");
        Check(sample.gpu_temperature_c==54 && sample.soc_temperature_c==56,"GPU and SoC temperature sources must be distinct");
        LinuxMetrics absent({fixtures/"available/proc",fixtures/"absent/sys",{}, {}});
        const auto none=absent.Sample();
        Check(none.memory && !none.gpu_frequency_hz && !none.gpu_busy && !none.gpu_temperature_c && !none.throttled,
            "Missing GPU/firmware permissions must preserve CPU/memory sampling without fake values");
        LinuxMetrics invalid({fixtures/"available/proc",fixtures/"invalid/sys",fixtures/"cpu_initial",{}});
        const auto rejected=invalid.Sample();
        Check(rejected.memory && !rejected.gpu_frequency_hz && !rejected.gpu_busy,
            "Invalid sysfs and unsupported firmware ioctl must degrade to unavailable");
        std::cout<<"metrics: CPU deltas, memory, real optional GPU/sensors and firmware parsing passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
