#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace prism::wm {
// Bounded samples; recording on the event thread never allocates or sorts.
// All durations are CPU wall time unless the caller supplies a KMS timestamp.
class TimingSamples {
public:
    struct Summary { std::size_t count{}; double mean{}, p50{}, p95{}, p99{}, maximum{}; };
    void Record(double milliseconds) {
        if (!std::isfinite(milliseconds) || milliseconds < 0) return;
        values_[next_] = milliseconds;
        next_ = (next_ + 1) % values_.size();
        size_ = std::min(size_ + 1, values_.size());
    }
    Summary Summarize() const {
        if (!size_) return {};
        auto sorted = values_;
        std::sort(sorted.begin(), sorted.begin() + size_);
        double sum{};
        for (std::size_t i=0;i<size_;++i) sum+=sorted[i];
        const auto percentile=[&](double fraction) {
            return sorted[static_cast<std::size_t>(std::ceil(fraction*size_))-1];
        };
        return {size_,sum/size_,percentile(.5),percentile(.95),percentile(.99),sorted[size_-1]};
    }
private:
    std::array<double,240> values_{};
    std::size_t next_{},size_{};
};
}
