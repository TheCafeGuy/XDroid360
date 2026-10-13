/**
 * XDroid360: couples guest time to frame completion.
 *
 * Guest time (mftb, timers, vblank interrupts) follows the host wall clock, so
 * a host that cannot finish a frame in the game's period (thermal throttling:
 * GPU 34 ms per 33.3 ms frame in mission 6) shows up to the game as a missed
 * deadline instead of a slower machine. AC6 then presents a frame without a
 * simulation step (HUD timer and speed equal to the previous frame, +2 ticks
 * on the next) and in that path its render-to-texture HUD panels come out
 * white or as other atlases (HUD-1). On the console guest time and work are
 * tied together, so the game never sees that.
 *
 * xdroid_time_coupling = 1: once the current frame is overdue (no VdSwap
 * xdroid_time_coupling_due_pct of the frame period after the last one, in
 * guest time), the guest clock runs at xdroid_time_coupling_slow_pct until
 * the swap comes (or xdroid_time_coupling_max_ms of wall time pass). The game
 * sees an on-time frame; the host just presents later: throttling lowers FPS
 * instead of breaking frames. The frame period is learned from the guest's
 * own swap intervals in vblanks (2 for a 30 FPS title at 60 Hz).
 */

#include "xdroid_time_coupling.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/gpu_flags.h"

DEFINE_int32(xdroid_time_coupling, 0,
             "XDroid360 (HUD-1): 1 = slow the guest clock while a frame is "
             "overdue so the game never sees a missed frame deadline "
             "(throttling lowers FPS instead of breaking frames). Live.",
             "XDroid360");
DEFINE_int32(xdroid_time_coupling_slow_pct, 10,
             "Guest clock speed in percent while a frame is overdue.",
             "XDroid360");
DEFINE_int32(xdroid_time_coupling_due_pct, 105,
             "A frame is overdue after this percent of the frame period.",
             "XDroid360");
DEFINE_int32(xdroid_time_coupling_max_ms, 40,
             "Longest wall time the guest clock stays slowed for one frame.",
             "XDroid360");

namespace xdroid {
namespace {

constexpr uint32_t kRing = 128;

struct State {
  std::mutex mutex;
  std::condition_variable wake;
  GuestSwapSample ring[kRing];
  uint64_t count = 0;  // swaps so far
  uint64_t period_us = 0;
  uint32_t period_vblanks = 0;
  bool slowed = false;
  uint64_t slowed_since_us = 0;
  TimeCouplingStats stats;
  bool monitor_started = false;
} g_state;

uint64_t WallUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

uint64_t GuestUs() {
  return uint64_t(double(xe::Clock::QueryGuestTickCount()) * 1000000.0 /
                  double(xe::Clock::guest_tick_frequency()));
}

// Under the lock.
void RestoreClock(uint64_t now_us) {
  if (!g_state.slowed) {
    return;
  }
  xe::Clock::set_guest_time_scalar(1.0);
  g_state.slowed = false;
  g_state.stats.slowed_us += now_us - g_state.slowed_since_us;
}

// Under the lock: the frame period from the median guest interval of recent
// swaps made at full clock speed, in whole vblanks.
void UpdatePeriod() {
  std::vector<uint64_t> intervals;
  const uint64_t n = std::min<uint64_t>(g_state.count, kRing);
  for (uint64_t i = 1; i < n && intervals.size() < 31; ++i) {
    const GuestSwapSample& now = g_state.ring[(g_state.count - i) % kRing];
    const GuestSwapSample& before =
        g_state.ring[(g_state.count - i - 1) % kRing];
    if (!now.slowed && now.guest_us > before.guest_us) {
      intervals.push_back(now.guest_us - before.guest_us);
    }
  }
  if (intervals.size() < 8) {
    return;
  }
  std::nth_element(intervals.begin(), intervals.begin() + intervals.size() / 2,
                   intervals.end());
  const uint64_t median = intervals[intervals.size() / 2];
  const uint64_t vblank_us = 1000000 / std::max(::GetGuestVblankRateHz(),
                                                uint32_t(1));
  const uint32_t vblanks =
      std::max<uint32_t>(1, uint32_t((median + vblank_us / 2) / vblank_us));
  g_state.period_vblanks = vblanks;
  g_state.period_us = vblanks * vblank_us;
}

void MonitorLoop() {
  std::unique_lock<std::mutex> lock(g_state.mutex);
  while (true) {
    if (cvars::xdroid_time_coupling != 1 || !g_state.period_us ||
        !g_state.count) {
      RestoreClock(WallUs());
      g_state.wake.wait_for(lock, std::chrono::milliseconds(100));
      continue;
    }
    const uint64_t swaps = g_state.count;
    const GuestSwapSample last = g_state.ring[(swaps - 1) % kRing];
    const uint64_t due_us =
        last.wall_us + g_state.period_us *
                           uint64_t(std::max(cvars::xdroid_time_coupling_due_pct,
                                             50)) /
                           100;
    const auto to_time_point = [](uint64_t us) {
      return std::chrono::steady_clock::time_point(
          std::chrono::microseconds(us));
    };
    g_state.wake.wait_until(lock, to_time_point(due_us), [&] {
      return g_state.count != swaps || cvars::xdroid_time_coupling != 1;
    });
    if (g_state.count != swaps || cvars::xdroid_time_coupling != 1) {
      continue;
    }
    // Overdue: slow the guest clock until the swap arrives.
    const uint64_t now = WallUs();
    const int slow_pct =
        std::clamp(cvars::xdroid_time_coupling_slow_pct, 1, 100);
    xe::Clock::set_guest_time_scalar(slow_pct / 100.0);
    g_state.slowed = true;
    g_state.slowed_since_us = now;
    ++g_state.stats.slowdowns;
    const uint64_t limit_us =
        now + uint64_t(std::max(cvars::xdroid_time_coupling_max_ms, 1)) * 1000;
    g_state.wake.wait_until(lock, to_time_point(limit_us), [&] {
      return g_state.count != swaps || cvars::xdroid_time_coupling != 1;
    });
    // The swap restores it itself; here only the bound or a switch-off.
    RestoreClock(WallUs());
    if (g_state.count == swaps) {
      // Still no swap: wait for it at full speed before arming again.
      g_state.wake.wait(lock, [&] {
        return g_state.count != swaps || cvars::xdroid_time_coupling != 1;
      });
    }
  }
}

}  // namespace

void TimeCouplingOnGuestSwap() {
  const uint64_t wall = WallUs();
  std::lock_guard<std::mutex> lock(g_state.mutex);
  const bool was_slowed = g_state.slowed;
  RestoreClock(wall);
  GuestSwapSample& sample = g_state.ring[g_state.count % kRing];
  sample.wall_us = wall;
  sample.guest_us = GuestUs();
  sample.slowed = was_slowed;
  if (g_state.count) {
    const GuestSwapSample& before = g_state.ring[(g_state.count - 1) % kRing];
    if (g_state.period_us &&
        sample.guest_us - before.guest_us > g_state.period_us * 3 / 2) {
      ++g_state.stats.late;
    }
  }
  ++g_state.count;
  ++g_state.stats.swaps;
  if (!(g_state.count & 15)) {
    UpdatePeriod();
  }
  if (!g_state.monitor_started) {
    g_state.monitor_started = true;
    // Detached: it lives as long as the process and only waits on |wake|.
    std::thread(MonitorLoop).detach();
  }
  g_state.wake.notify_one();
}

uint32_t RecentGuestSwaps(GuestSwapSample* out, uint32_t max) {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  const uint32_t n =
      uint32_t(std::min<uint64_t>({g_state.count, uint64_t(kRing), max}));
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = g_state.ring[(g_state.count - n + i) % kRing];
  }
  return n;
}

TimeCouplingStats GetTimeCouplingStats() {
  std::lock_guard<std::mutex> lock(g_state.mutex);
  TimeCouplingStats stats = g_state.stats;
  stats.period_vblanks = g_state.period_vblanks;
  return stats;
}

}  // namespace xdroid
