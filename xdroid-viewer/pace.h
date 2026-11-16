// MIRROR-1 viewer: frame pacing for vsync (F6). A picture is due once the
// present deadline passes its phone time plus the slowest arrival delay of the
// last 1-2 s, so the screen replays the phone's frame times (30 fps on 60 Hz:
// 2, 2, 2 refreshes, not the 1, 3 that arrival jitter gives) with the least
// delay that still has every picture in time. The schedule is absolute: a late
// picture costs its own refresh only, never shifts the ones after it. A due
// picture is shown the moment it lands; one within a quarter refresh of its
// deadline keeps the side its cadence gives, so timestamp jitter cannot flip it.
#pragma once

#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "common.h"
#include "decode.h"

namespace xv {

class Pacer {
 public:
  void SetPeriod(int64_t period_us) { period_us_ = double(period_us); }
  double guard_us() const { return guard_us_; }
  double period_us() const { return period_us_; }

  // After the swapchain's waitable fired: DXGI's last vblank (zero before the
  // first one). Picks the next vblank a present can still make and returns
  // its deadline.
  int64_t Deadline(const DXGI_FRAME_STATISTICS& sync, int64_t now_us) {
    if (!sync.SyncRefreshCount) {
      vblank_us_ = double(now_us) + guard_us_;  // No vblank seen yet.
      return now_us;
    }
    if (sync_.SyncRefreshCount) {
      uint32_t refreshes = sync.SyncRefreshCount - sync_.SyncRefreshCount;
      uint32_t presents = sync.PresentCount - sync_.PresentCount;
      if (refreshes && refreshes < 8) {
        double sample = double(QpcToUs(sync.SyncQPCTime.QuadPart) -
                               QpcToUs(sync_.SyncQPCTime.QuadPart)) /
                        refreshes;
        if (std::abs(sample - period_us_) < period_us_ / 8) {
          period_us_ += (sample - period_us_) / 16;
        }
        // A refresh went by without a new present: ours came too late.
        guard_us_ = presents && refreshes > presents
                        ? std::min(guard_us_ + 1000, period_us_ / 2)
                        : std::max(guard_us_ - 2, kMinGuardUs);
      }
    }
    sync_ = sync;
    double last = double(QpcToUs(sync.SyncQPCTime.QuadPart));
    vblank_us_ = last + period_us_ * std::max(1.0, std::ceil((now_us + guard_us_ - last) /
                                                             period_us_));
    return int64_t(vblank_us_ - guard_us_);
  }

  // shown: the picture on screen (nullptr before the first); next: the
  // oldest queued one (nullptr when none). Returns how many pictures to step
  // for the vblank Deadline() picked, 0 to hold.
  uint32_t Step(const FrameTimes* shown, const FrameTimes* next,
                uint32_t queued) {
    if (!next) {
      return 0;
    }
    int64_t gap = shown ? next->pts_us - shown->pts_us : 0;
    if (gap <= 0 || gap > 1000000 || queued > 2) {
      // First picture, a stall's backlog or a phone clock jump: the newest
      // now, and arrival delays measured afresh.
      peak_[0] = peak_[1] = INT64_MIN;
      window_us_ = shown_vblank_us_ = vblank_us_;
      return queued;
    }
    int64_t peak = std::max(peak_[0], peak_[1]);
    if (peak != INT64_MIN) {
      double cadence =
          shown_vblank_us_ + period_us_ * std::round(gap / period_us_);
      double stick = vblank_us_ + period_us_ / 2 >= cadence ? kStick : -kStick;
      if (double(next->pts_us + peak) >
          vblank_us_ - guard_us_ + stick * period_us_) {
        return 0;
      }
    }
    // A late picture raises the delay by at most a refresh: a USB stall's
    // burst is not jitter worth waiting for.
    int64_t delay = next->decoded_us - next->pts_us;
    peak_[1] = std::max(peak_[1], peak == INT64_MIN
                                      ? delay
                                      : std::min(delay, peak + int64_t(period_us_)));
    if (vblank_us_ - window_us_ >= 1000000) {
      peak_[0] = peak_[1];
      peak_[1] = INT64_MIN;
      window_us_ = vblank_us_;
    }
    shown_vblank_us_ = vblank_us_;
    return 1;
  }

 private:
  static constexpr double kMinGuardUs = 1500;
  static constexpr double kStick = 0.25;  // Of a refresh.

  double period_us_ = 16667;
  double guard_us_ = 3000;
  DXGI_FRAME_STATISTICS sync_{};
  double vblank_us_ = 0;        // The vblank Deadline() picked.
  double shown_vblank_us_ = 0;  // The one the shown picture went to.
  double window_us_ = 0;
  // Max arrival delay (decoded minus phone pts) of the last and this 1 s.
  int64_t peak_[2] = {INT64_MIN, INT64_MIN};
};

}  // namespace xv
