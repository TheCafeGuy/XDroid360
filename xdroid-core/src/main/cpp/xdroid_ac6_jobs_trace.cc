/**
 * XDroid360 forensic (HUD-1, temp, revert-planned): AC6 job pool B tracer.
 *
 * Every 10 s, per job class: jobs, submit-to-start latency (p50/p99/max from
 * log2 buckets), duration, jobs whose callers stored into them after Submit
 * (race 2 class), jobs still running when the CP executed later commands
 * (cp_late), arm waits/timeouts, and the guest swap lag histogram. On each
 * glitch recorder detection of frame n, a summary of the jobs submitted in
 * frames n-3..n+1 and, for the first detections, one line per job.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "xdroid_ac6_jobs.h"
#include "xdroid_time_coupling.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_bool(xdroid_job_tracer, true,
            "XDroid360 forensic (HUD-1): log AC6 job pool timing every 10 s "
            "and around glitch recorder detections.",
            "XDroid360");

namespace xdroid {
namespace {

constexpr uint32_t kBuckets = 24;  // log2 of microseconds
std::atomic<uint32_t> g_latency[kAc6MaxClasses][kBuckets];
std::atomic<uint64_t> g_latency_max[kAc6MaxClasses];
std::atomic<uint32_t> g_glitches{0};

uint32_t Bucket(uint64_t us) {
  uint32_t bucket = 0;
  while (us > 1 && bucket < kBuckets - 1) {
    us >>= 1;
    ++bucket;
  }
  return bucket;
}

void OnStart(const Ac6JobRecord& record, uint64_t latency_us) {
  if (!cvars::xdroid_job_tracer || record.cls >= kAc6MaxClasses) {
    return;
  }
  g_latency[record.cls][Bucket(latency_us)].fetch_add(
      1, std::memory_order_relaxed);
  uint64_t max = g_latency_max[record.cls].load(std::memory_order_relaxed);
  while (latency_us > max && !g_latency_max[record.cls].compare_exchange_weak(
                                 max, latency_us, std::memory_order_relaxed)) {
  }
}

void OnGlitch(uint32_t frame) {
  if (!cvars::xdroid_job_tracer) {
    return;
  }
  const uint32_t glitch = g_glitches.fetch_add(1) + 1;
  const uint32_t last = Ac6JobLastSeq();
  uint32_t jobs = 0, late_start = 0, cp_late = 0, post_stores = 0;
  uint64_t max_latency = 0;
  std::string detail;
  uint32_t detail_lines = 0;
  for (uint32_t back = 0; back < kAc6JobRing - 16 && back < last; ++back) {
    const Ac6JobRecord* record = Ac6JobRecordAt(last - back);
    if (!record || record->submit_frame + 3 < frame ||
        record->submit_frame > frame + 1) {
      continue;
    }
    ++jobs;
    const uint64_t start = record->start_us.load(std::memory_order_acquire);
    const uint64_t end = record->end_us.load(std::memory_order_acquire);
    const uint64_t latency = start ? start - record->submit_us : 0;
    max_latency = std::max(max_latency, latency);
    late_start += latency > 2000;
    const bool late = record->cp_late.load(std::memory_order_relaxed);
    cp_late += late;
    const uint64_t mask = record->post_store_valid ? record->post_store_mask
                                                   : 0;
    post_stores += mask != 0;
    // The HUD class (pool B, formatPlayer.xml) stays detailed past the first
    // 40 detections: HUD-1 only shows up after minutes of throttling.
    if ((glitch <= 40 || record->vt == 0x8205574C) && detail_lines < 48 &&
        (late || mask || latency > 1000)) {
      ++detail_lines;
      detail += fmt::format(
          "\n  XDROID-JOBS f{} vt {:08X} fn {:08X} site {:08X} sf {} lat {} "
          "dur {} cp_late {} post {:016X}",
          frame, record->vt, record->fn, record->site, record->submit_frame,
          latency, end && start ? end - start : 0, int(late), mask);
    }
  }
  // H-REDRAW: was a guest frame late (guest interval > 1.5 periods) right
  // before the detection? Newest last, guest ms / wall ms per swap.
  GuestSwapSample swaps[12];
  const uint32_t count = RecentGuestSwaps(swaps, 12);
  std::string intervals;
  for (uint32_t i = 1; i < count; ++i) {
    intervals += fmt::format(
        " {:.1f}/{:.1f}{}", (swaps[i].guest_us - swaps[i - 1].guest_us) / 1000.0,
        (swaps[i].wall_us - swaps[i - 1].wall_us) / 1000.0,
        swaps[i].slowed ? "s" : "");
  }
  XELOGW(
      "XDROID-JOBS: glitch frame {}: {} jobs in n-3..n+1, {} started >2 ms "
      "late (max {} us), {} cp_late, {} with caller stores after Submit; "
      "last swaps guest/wall ms:{}{}",
      frame, jobs, late_start, max_latency, cp_late, post_stores, intervals,
      detail);
}

void SummaryLoop() {
  std::array<uint64_t, kAc6MaxClasses> last_jobs{};
  std::array<uint64_t, kAc6MaxClasses> last_post{};
  std::array<uint64_t, kAc6MaxClasses> last_late{};
  std::array<uint64_t, kAc6MaxClasses> last_waits{};
  std::array<uint64_t, kAc6MaxClasses> last_timeouts{};
  std::array<uint64_t, kAc6MaxClasses> last_cp_waits{};
  std::array<uint64_t, kAc6MaxClasses> last_cp_timeouts{};
  uint64_t last_lag[4] = {};
  uint64_t last_wait[3] = {};
  uint64_t last_defer[3] = {};
  TimeCouplingStats last_coupling;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    if (!cvars::xdroid_job_tracer) {
      continue;
    }
    std::string lines;
    for (uint32_t i = 0; i < kAc6MaxClasses; ++i) {
      Ac6JobClass* job_class = Ac6JobClassAt(i);
      if (!job_class) {
        break;
      }
      const uint64_t jobs = job_class->jobs.load(std::memory_order_relaxed);
      const uint64_t window = jobs - last_jobs[i];
      uint32_t counts[kBuckets];
      uint64_t total = 0;
      for (uint32_t b = 0; b < kBuckets; ++b) {
        counts[b] = g_latency[i][b].exchange(0, std::memory_order_relaxed);
        total += counts[b];
      }
      const uint64_t max =
          g_latency_max[i].exchange(0, std::memory_order_relaxed);
      auto percentile = [&](uint64_t per_mille) -> uint64_t {
        uint64_t seen = 0;
        for (uint32_t b = 0; b < kBuckets; ++b) {
          seen += counts[b];
          if (seen * 1000 >= total * per_mille) {
            return uint64_t(1) << b;
          }
        }
        return 0;
      };
      const uint64_t post = job_class->post_store_jobs.load();
      const uint64_t late = job_class->cp_late_jobs.load();
      const uint64_t waits = job_class->start_waits.load();
      const uint64_t timeouts = job_class->start_timeouts.load();
      const uint64_t cp_waits = job_class->cp_waits.load();
      const uint64_t cp_timeouts = job_class->cp_timeouts.load();
      if (window) {
        lines += fmt::format(
            "\n  vt {:08X} fn {:08X}: {} jobs, start lat p50 <{} p99 <{} max "
            "{} us, dur ~{} us, post-store {} (mask {:016X}), cp_late {}, "
            "A wait {}/{} to, C+ wait {}/{} to{}{}{}",
            job_class->vt.load(), job_class->fn.load(), window,
            percentile(500), percentile(990), max,
            job_class->duration_ewma_us.load(), post - last_post[i],
            job_class->post_store_mask.load(), late - last_late[i],
            waits - last_waits[i], timeouts - last_timeouts[i],
            cp_waits - last_cp_waits[i], cp_timeouts - last_cp_timeouts[i],
            job_class->storage.load() ? " storage" : "",
            job_class->no_start_wait.load() ? " noA" : "",
            job_class->no_cp_wait.load() ? " noC" : "");
      }
      last_jobs[i] = jobs;
      last_post[i] = post;
      last_late[i] = late;
      last_waits[i] = waits;
      last_timeouts[i] = timeouts;
      last_cp_waits[i] = cp_waits;
      last_cp_timeouts[i] = cp_timeouts;
    }
    const TimeCouplingStats coupling = GetTimeCouplingStats();
    XELOGI(
        "XDROID-JOBS: 10 s: guest frames {} late {} (period {} vblanks), "
        "clock slowed {} times for {} ms; job resubmitted before start {} "
        "(total)",
        coupling.swaps - last_coupling.swaps, coupling.late - last_coupling.late,
        coupling.period_vblanks, coupling.slowdowns - last_coupling.slowdowns,
        (coupling.slowed_us - last_coupling.slowed_us) / 1000,
        Ac6JobResubmits());
    last_coupling = coupling;
    uint64_t lag[4], wait[3], defer[3];
    Ac6SwapLagHistogram(lag);
    Ac6SwapWaitStats(wait);
    Ac6CpDeferStats(defer);
    XELOGI(
        "XDROID-JOBS: 10 s: guest swaps with CP behind by 0/1/2/3+: "
        "{}/{}/{}/{}; C- waits {} timeouts {} waited {} ms; H1 waits {} "
        "timeouts {} waited {} ms{}",
        lag[0] - last_lag[0], lag[1] - last_lag[1], lag[2] - last_lag[2],
        lag[3] - last_lag[3], wait[0] - last_wait[0], wait[1] - last_wait[1],
        (wait[2] - last_wait[2]) / 1000, defer[0] - last_defer[0],
        defer[1] - last_defer[1], (defer[2] - last_defer[2]) / 1000, lines);
    std::copy(defer, defer + 3, last_defer);
    std::copy(lag, lag + 4, last_lag);
    std::copy(wait, wait + 3, last_wait);
  }
}

}  // namespace

void InstallAc6JobTracer() {
  static std::once_flag once;
  std::call_once(once, [] {
    Ac6JobObservers observers;
    observers.on_start = OnStart;
    observers.on_glitch = OnGlitch;
    SetAc6JobObservers(observers);
    // Detached: it only sleeps and logs, for the life of the process.
    std::thread(SummaryLoop).detach();
    XELOGI("XDroid360: AC6 job tracer installed");
  });
}

}  // namespace xdroid
