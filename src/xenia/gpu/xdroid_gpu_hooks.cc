/**
 * XDroid360 F1 XDROID-LAZY: counts and logs lazy host work that may block the
 * CP thread (G2-3 stall). Kept out of command_processor.cc (rule 10).
 */

#include "xenia/gpu/xdroid_gpu_hooks.h"

#include <iterator>
#include <string>

#include "xenia/base/logging.h"

namespace xe {
namespace gpu {

namespace {
struct XdroidLazyStats {
  std::atomic<uint32_t> count{0};
  std::atomic<uint64_t> sum_ns{0};
  std::atomic<uint64_t> max_ns{0};
  std::atomic<uint32_t> lines{0};
};
XdroidLazyStats xdroid_lazy_stats[size_t(XdroidLazyKind::kCount)];
constexpr const char* kXdroidLazyNames[] = {"rt",     "wide", "rpass", "fb",
                                            "xfer",   "dump", "tex",   "wait",
                                            "texmem", "rtmem"};
static_assert(std::size(kXdroidLazyNames) == size_t(XdroidLazyKind::kCount));
}  // namespace

void XdroidLazyNote(XdroidLazyKind kind, uint64_t ns, uint64_t bytes) {
  if (ns <= 1000000) {
    return;
  }
  XdroidLazyStats& stats = xdroid_lazy_stats[size_t(kind)];
  stats.count.fetch_add(1, std::memory_order_relaxed);
  stats.sum_ns.fetch_add(ns, std::memory_order_relaxed);
  uint64_t max_ns = stats.max_ns.load(std::memory_order_relaxed);
  while (ns > max_ns && !stats.max_ns.compare_exchange_weak(
                            max_ns, ns, std::memory_order_relaxed)) {
  }
  if ((kind != XdroidLazyKind::kWait || ns >= 50000000) &&
      stats.lines.fetch_add(1, std::memory_order_relaxed) < 25) {
    if (bytes) {
      XELOGI("XDroid360: XDROID-LAZY kind={} ms={:.1f} mb={:.1f} frame={}",
             kXdroidLazyNames[size_t(kind)], ns * 1e-6,
             bytes * (1.0 / (1024 * 1024)), logging::GetFrameNumber());
    } else {
      XELOGI("XDroid360: XDROID-LAZY kind={} ms={:.1f} frame={}",
             kXdroidLazyNames[size_t(kind)], ns * 1e-6,
             logging::GetFrameNumber());
    }
  }
}

void XdroidLazyFlush() {
  static auto window_start = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - window_start < std::chrono::seconds(10)) {
    return;
  }
  window_start = now;
  std::string line;
  for (size_t i = 0; i < size_t(XdroidLazyKind::kCount); ++i) {
    XdroidLazyStats& stats = xdroid_lazy_stats[i];
    uint32_t count = stats.count.exchange(0, std::memory_order_relaxed);
    if (count) {
      line += fmt::format(
          " {}={}/{:.1f}/{:.1f}", kXdroidLazyNames[i], count,
          stats.sum_ns.exchange(0, std::memory_order_relaxed) * 1e-6,
          stats.max_ns.exchange(0, std::memory_order_relaxed) * 1e-6);
    }
  }
  if (!line.empty()) {
    XELOGI("XDroid360: XDROID-LAZY sum{} frame={}", line,
           logging::GetFrameNumber());
  }
}

}  // namespace gpu
}  // namespace xe
