/**
 * XDroid360: couples guest time to frame completion (HUD-1, throttle rule).
 */

#ifndef XDROID_TIME_COUPLING_H_
#define XDROID_TIME_COUPLING_H_

#include <cstdint>

namespace xdroid {

// Call at every guest VdSwap (guest thread).
void TimeCouplingOnGuestSwap();

// Guest swaps by index, newest last: wall and guest time of each.
struct GuestSwapSample {
  uint64_t wall_us = 0;
  uint64_t guest_us = 0;
  bool slowed = false;  // the guest clock was slowed before this swap
};
// Copies up to |max| newest samples into |out|, oldest first; returns count.
uint32_t RecentGuestSwaps(GuestSwapSample* out, uint32_t max);

struct TimeCouplingStats {
  uint64_t swaps = 0;
  uint64_t late = 0;       // guest interval > 1.5 frame periods
  uint64_t slowdowns = 0;  // times the guest clock was slowed
  uint64_t slowed_us = 0;  // wall time spent slowed
  uint32_t period_vblanks = 0;
};
TimeCouplingStats GetTimeCouplingStats();

}  // namespace xdroid

#endif  // XDROID_TIME_COUPLING_H_
