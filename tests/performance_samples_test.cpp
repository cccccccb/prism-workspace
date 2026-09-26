#include "prism/wm/performance.hpp"
#include <cassert>
#include <limits>
int main() {
    prism::wm::TimingSamples samples;
    assert(samples.Summarize().count==0);
    samples.Record(-1); samples.Record(std::numeric_limits<double>::infinity());
    assert(samples.Summarize().count==0);
    for(int i=1;i<=300;++i) samples.Record(i);
    const auto result=samples.Summarize();
    assert(result.count==240 && result.mean==180.5);
    assert(result.p50==180 && result.p95==288 && result.p99==298 && result.maximum==300);
}
