#pragma once
#include "transform.hpp"
#include <deque>
#include <optional>

namespace bridge {
// Runs on the game thread. Packet timestamps interpolate within one sender's
// clock; freshness uses local receipt time, so Wine-prefix clocks need not match.
class Timeline {
  std::deque<StatePacket> samples_;
  std::deque<std::uint64_t> retired_;
  std::uint64_t last_receipt_{};
  std::deque<long double> offsets_;
  long double clock_offset_{};
public:
  void clear() { samples_.clear(); retired_.clear(); last_receipt_=0; offsets_.clear(); clock_offset_=0; }
  bool push(const StatePacket& p, std::uint64_t now_us) {
    if (!valid(p)) return false;
    if (!samples_.empty() && p.header.session != samples_.back().header.session) {
      if (std::find(retired_.begin(),retired_.end(),p.header.session)!=retired_.end()) return false;
      retired_.push_back(samples_.back().header.session);
      if (retired_.size()>8) retired_.pop_front();
      samples_.clear(); offsets_.clear();
    }
    if (!samples_.empty() && (!newer(p.header.sequence,samples_.back().header.sequence) ||
      p.header.timestamp_us <= samples_.back().header.timestamp_us)) return false;
    const auto offset=static_cast<long double>(now_us)-p.header.timestamp_us;
    offsets_.push_back(offset);if(offsets_.size()>128)offsets_.pop_front();
    // Minimum arrival delay rejects scheduling spikes; slow slew tracks clock
    // drift without moving the playback clock each time a packet arrives.
    const auto best=*std::min_element(offsets_.begin(),offsets_.end());
    if(samples_.empty())clock_offset_=best;
    else clock_offset_+=std::clamp(best-clock_offset_,-50.0L,50.0L);
    samples_.push_back(p); last_receipt_=now_us;
    if (samples_.size()>64) samples_.pop_front();
    return true;
  }
  const StatePacket* latest() const { return samples_.empty() ? nullptr : &samples_.back(); }
  std::uint64_t prediction_age_us(std::uint64_t now, std::uint64_t delay=30000) const {
    if(samples_.empty())return 0;
    const auto age=static_cast<long double>(now)-clock_offset_-delay-samples_.back().header.timestamp_us;
    return age>0?static_cast<std::uint64_t>(age):0;
  }
  std::optional<StatePacket> sample(std::uint64_t now_us, std::uint64_t delay_us=30000,
    std::uint64_t timeout_us=500000, std::uint64_t prediction_us=20000) const {
    if (samples_.empty() || now_us < last_receipt_ || now_us-last_receipt_ > timeout_us) return {};
    const auto& last=samples_.back();
    const auto target=static_cast<long double>(now_us)-clock_offset_-delay_us;
    if (target <= samples_.front().header.timestamp_us) return samples_.front();
    for (std::size_t i=1;i<samples_.size();++i) {
      if (target <= samples_[i].header.timestamp_us) {
        const auto& a=samples_[i-1]; const auto& b=samples_[i];
        const auto t=static_cast<float>((target-a.header.timestamp_us)/(b.header.timestamp_us-a.header.timestamp_us));
        return interpolate(a,b,std::clamp(t,0.0f,1.0f));
      }
    }
    auto predicted=last;
    if(samples_.size()<2 || !prediction_us)return predicted;
    const auto& previous=samples_[samples_.size()-2];
    if((previous.flags&7u)!=(last.flags&7u) || length(previous.car.position-last.car.position)>1000)return last;
    const auto horizon=std::min(target-last.header.timestamp_us,static_cast<long double>(prediction_us));
    const auto seconds=static_cast<float>(horizon/1000000.0L);
    const auto rotation_t=1.0f+static_cast<float>(horizon/(last.header.timestamp_us-previous.header.timestamp_us));
    const auto delta=last.car.velocity*seconds;
    predicted.car.position=last.car.position+delta;
    predicted.car.rotation=nlerp(previous.car.rotation,last.car.rotation,rotation_t);
    // Keep the camera's relative offset fixed through a short packet gap.
    predicted.camera.position=last.camera.position+delta;
    if(length(previous.ball.position-last.ball.position)<=1000) {
      predicted.ball.position=last.ball.position+last.ball.velocity*seconds;
      predicted.ball.rotation=nlerp(previous.ball.rotation,last.ball.rotation,rotation_t);
    }
    return predicted; // visual only; native collisions always use latest()
  }
};
}
