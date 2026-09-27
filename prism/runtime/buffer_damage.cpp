#include "prism/runtime/buffer_damage.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace prism::runtime {
namespace {
using Rect=contracts::DamageRect;
using Region=contracts::DamageRegion;
using Size=contracts::BufferSize;
std::uint64_t TargetArea(Size size) {
    return std::uint64_t(size.width)*size.height;
}
bool Representable(Size size) {
    return size.width>0 && size.height>0 && size.width<=INT32_MAX && size.height<=INT32_MAX;
}
DamageLimits ValidateLimits(DamageLimits limits) {
    if(!limits.max_rects || limits.max_rects>4096 || !std::isfinite(limits.max_area_ratio) ||
        limits.max_area_ratio<=0 || limits.max_area_ratio>1)
        throw std::invalid_argument("Invalid buffer damage limits");
    return limits;
}
std::size_t ValidateCapacity(std::size_t capacity) {
    if(!capacity || capacity>4096)throw std::invalid_argument("Invalid buffer damage history capacity");
    return capacity;
}
std::int64_t Right(Rect rect) {return std::int64_t(rect.x)+rect.width;}
std::int64_t Bottom(Rect rect) {return std::int64_t(rect.y)+rect.height;}
std::optional<Rect> Clip(Rect rect,Size size) {
    if(rect.width<=0 || rect.height<=0)return {};
    const auto left=std::max<std::int64_t>(0,rect.x),top=std::max<std::int64_t>(0,rect.y);
    const auto right=std::min<std::int64_t>(size.width,Right(rect));
    const auto bottom=std::min<std::int64_t>(size.height,Bottom(rect));
    if(right<=left || bottom<=top)return {};
    return Rect{static_cast<std::int32_t>(left),static_cast<std::int32_t>(top),
        static_cast<std::int32_t>(right-left),static_cast<std::int32_t>(bottom-top)};
}
void Push(std::vector<Rect>& result,std::int64_t x,std::int64_t y,std::int64_t right,std::int64_t bottom) {
    if(right>x && bottom>y)result.push_back({static_cast<std::int32_t>(x),static_cast<std::int32_t>(y),
        static_cast<std::int32_t>(right-x),static_cast<std::int32_t>(bottom-y)});
}
// Keep an exact, non-overlapping union. Subtract previous coverage before
// adding a rectangle rather than replacing an L-shaped union with its bounds.
void Subtract(Rect source,Rect covered,std::vector<Rect>& result) {
    const auto x=std::max(source.x,covered.x),y=std::max(source.y,covered.y);
    const auto right=std::min(Right(source),Right(covered)),bottom=std::min(Bottom(source),Bottom(covered));
    if(right<=x || bottom<=y){result.push_back(source);return;}
    Push(result,source.x,source.y,Right(source),y);
    Push(result,source.x,bottom,Right(source),Bottom(source));
    Push(result,source.x,y,x,bottom);
    Push(result,right,y,Right(source),bottom);
}
void Coalesce(std::vector<Rect>& rects) {
    for(bool changed=true;changed;) {
        changed=false;
        for(std::size_t i=0;i<rects.size() && !changed;++i)
            for(std::size_t j=i+1;j<rects.size();++j) {
                auto& a=rects[i];const auto& b=rects[j];
                if(a.y==b.y && a.height==b.height && (Right(a)==b.x || Right(b)==a.x)) {
                    const auto right=std::max(Right(a),Right(b));a.x=std::min(a.x,b.x);
                    a.width=static_cast<std::int32_t>(right-a.x);
                } else if(a.x==b.x && a.width==b.width && (Bottom(a)==b.y || Bottom(b)==a.y)) {
                    const auto bottom=std::max(Bottom(a),Bottom(b));a.y=std::min(a.y,b.y);
                    a.height=static_cast<std::int32_t>(bottom-a.y);
                } else continue;
                rects.erase(rects.begin()+j);changed=true;break;
            }
    }
}
std::uint64_t DisjointArea(const std::vector<Rect>& rects) {
    std::uint64_t area=0;
    for(const auto& rect:rects)area+=std::uint64_t(rect.width)*rect.height;
    return area;
}
class Normalizer {
public:
    Normalizer(Size size,DamageLimits limits):size_(size),limits_(ValidateLimits(limits)) {}
    bool Add(const Region& region) {
        if(region.full)return false;
        for(const auto& input:region.rects) {
            if(input.width<0 || input.height<0)return false;
            const auto clipped=Clip(input,size_);if(!clipped)continue;
            std::vector<Rect> pending{*clipped};
            for(const auto& covered:rects_) {
                std::vector<Rect> uncovered;
                for(const auto& piece:pending) {
                    Subtract(piece,covered,uncovered);
                    if(uncovered.size()>limits_.max_rects*4)return false;
                }
                pending=std::move(uncovered);if(pending.empty())break;
            }
            rects_.insert(rects_.end(),pending.begin(),pending.end());
            Coalesce(rects_);
            if(rects_.size()>limits_.max_rects)return false;
            const auto area=DisjointArea(rects_),total=TargetArea(size_);
            if(area==total || static_cast<long double>(area)>
                static_cast<long double>(total)*limits_.max_area_ratio)return false;
        }
        return true;
    }
    Region Finish() {
        std::sort(rects_.begin(),rects_.end(),[](Rect a,Rect b) {
            if(a.y!=b.y)return a.y<b.y;
            if(a.x!=b.x)return a.x<b.x;
            if(a.height!=b.height)return a.height<b.height;
            return a.width<b.width;
        });
        return {false,std::move(rects_)};
    }
private:
    Size size_;DamageLimits limits_;std::vector<Rect> rects_;
};
} // namespace

contracts::DamageRegion NormalizeDamage(const Region& region,Size size,DamageLimits limits) {
    ValidateLimits(limits);
    if(!size.width || !size.height)return {};
    if(!Representable(size))return Region::Full();
    Normalizer normalized(size,limits);
    return normalized.Add(region)?normalized.Finish():Region::Full();
}
contracts::DamageRegion UnionDamage(std::span<const Region> regions,Size size,DamageLimits limits) {
    ValidateLimits(limits);
    if(!size.width || !size.height)return {};
    if(!Representable(size))return Region::Full();
    Normalizer normalized(size,limits);
    for(const auto& region:regions)if(!normalized.Add(region))return Region::Full();
    return normalized.Finish();
}
contracts::DamageRegion UnionDamage(const Region& a,const Region& b,Size size,DamageLimits limits) {
    ValidateLimits(limits);
    if(!size.width || !size.height)return {};
    if(!Representable(size))return Region::Full();
    Normalizer normalized(size,limits);
    return normalized.Add(a) && normalized.Add(b)?normalized.Finish():Region::Full();
}
std::uint64_t DamageArea(const Region& region,Size size) {
    if(region.full)return TargetArea(size);
    std::vector<Rect> rects;std::vector<std::int64_t> edges;
    rects.reserve(region.rects.size());
    for(const auto& input:region.rects) {
        if(input.width<0 || input.height<0)return TargetArea(size);
        if(const auto rect=Clip(input,size)) {
            rects.push_back(*rect);edges.push_back(rect->y);edges.push_back(Bottom(*rect));
        }
    }
    std::sort(edges.begin(),edges.end());edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
    std::uint64_t area=0;
    std::vector<std::pair<std::int64_t,std::int64_t>> intervals;intervals.reserve(rects.size());
    for(std::size_t row=1;row<edges.size();++row) {
        intervals.clear();const auto top=edges[row-1],bottom=edges[row];
        for(const auto rect:rects)if(rect.y<=top && Bottom(rect)>=bottom)intervals.emplace_back(rect.x,Right(rect));
        std::sort(intervals.begin(),intervals.end());
        std::int64_t width=0,left=0,right=0;bool started=false;
        for(const auto& interval:intervals) {
            if(!started){left=interval.first;right=interval.second;started=true;}
            else if(interval.first<=right)right=std::max(right,interval.second);
            else {width+=right-left;left=interval.first;right=interval.second;}
        }
        if(started)width+=right-left;
        area+=std::uint64_t(width)*std::uint64_t(bottom-top);
    }
    return area;
}
contracts::DamageRegion DamageFromLogicalBounds(std::span<const contracts::LogicalRect> bounds,
    Size size,double scale,DamageLimits limits) {
    ValidateLimits(limits);
    if(!std::isfinite(scale) || scale<=0)throw std::invalid_argument("Invalid buffer damage scale");
    if(!size.width || !size.height)return {};
    if(!Representable(size))return Region::Full();
    Normalizer normalized(size,limits);
    for(const auto& bound:bounds) {
        if(!std::isfinite(bound.x) || !std::isfinite(bound.y) || !std::isfinite(bound.width) ||
            !std::isfinite(bound.height) || bound.width<0 || bound.height<0)return Region::Full();
        if(bound.width==0 || bound.height==0)continue;
        const long double left=static_cast<long double>(bound.x)*scale;
        const long double top=static_cast<long double>(bound.y)*scale;
        const long double right=(static_cast<long double>(bound.x)+bound.width)*scale;
        const long double bottom=(static_cast<long double>(bound.y)+bound.height)*scale;
        if(!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom))return Region::Full();
        const auto x=std::clamp(std::floor(left),0.0L,static_cast<long double>(size.width));
        const auto y=std::clamp(std::floor(top),0.0L,static_cast<long double>(size.height));
        const auto x1=std::clamp(std::ceil(right),0.0L,static_cast<long double>(size.width));
        const auto y1=std::clamp(std::ceil(bottom),0.0L,static_cast<long double>(size.height));
        Region region;
        if(x1>x && y1>y)region.rects.push_back({static_cast<std::int32_t>(x),static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(x1-x),static_cast<std::int32_t>(y1-y)});
        if(!normalized.Add(region))return Region::Full();
    }
    return normalized.Finish();
}

BufferDamageHistory::BufferDamageHistory(std::size_t capacity,DamageLimits limits)
    :history_(ValidateCapacity(capacity)),limits_(ValidateLimits(limits)) {
    static_assert(std::is_nothrow_move_assignable_v<Region>);
}
void BufferDamageHistory::Reset(contracts::BufferSize size) {
    if(!Representable(size))throw std::invalid_argument("Invalid buffer damage target size");
    Invalidate();size_=size;
}
void BufferDamageHistory::Invalidate() noexcept {
    history_valid_=false;history_size_=next_index_=0;++epoch_;
    for(auto& delta:history_){delta.full=false;delta.rects.clear();}
}
BufferDamagePlan BufferDamageHistory::Plan(const Region& content,std::optional<unsigned> age) const {
    if(!Representable(size_))throw std::logic_error("Buffer damage target is not configured");
    if(successful_sequence_==std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Buffer damage sequence exhausted");
    BufferDamagePlan plan{size_,epoch_,successful_sequence_+1,NormalizeDamage(content,size_,limits_),{}};
    if(!history_valid_ || !age || !*age || *age>history_size_+1) {
        plan.repair_damage=Region::Full();return plan;
    }
    plan.repair_damage=plan.content_damage;
    for(unsigned previous=0;previous+1<*age;++previous) {
        const auto index=(next_index_+history_.size()-1-previous)%history_.size();
        plan.repair_damage=UnionDamage(plan.repair_damage,history_[index],size_,limits_);
        if(plan.repair_damage.full)break;
    }
    return plan;
}
bool BufferDamageHistory::Commit(BufferDamagePlan&& plan) noexcept {
    if(plan.epoch!=epoch_ || plan.next_sequence!=successful_sequence_+1 ||
        plan.size.width!=size_.width || plan.size.height!=size_.height ||
        !Representable(size_) || (plan.content_damage.full && !plan.content_damage.rects.empty()))return false;
    history_[next_index_]=std::move(plan.content_damage);
    next_index_=(next_index_+1)%history_.size();
    history_size_=std::min(history_size_+1,history_.size());
    successful_sequence_=plan.next_sequence;history_valid_=true;
    return true;
}
} // namespace prism::runtime
