// XDroid360 LAT-1: input and frame latency meter. See xdroid_latency.h.
#include "xdroid_latency.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/xdroid_gpu_hooks.h"
#include "xenia/ui/xdroid_ui_hooks.h"

DEFINE_int32(xdroid_lat_trace, 0,
             "LAT-1: log the timeline of the next N presented frames "
             "(XDROID-LATF: kicks, CP start, CP swap, present, relative to the "
             "previous VdSwap). Live.",
             "XDroid360");

DEFINE_bool(xdroid_lat_meter, true,
            "LAT-1: run the latency meter (pad, kick, CP and present hooks). "
            "Live.",
            "XDroid360");

namespace xdroid {

namespace {

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

constexpr uint32_t kRing = 64;
constexpr uint32_t kWindow = 300;

std::atomic<uint64_t> g_guest_swaps{0};
std::atomic<uint64_t> g_cp_swaps{0};
std::atomic<uint64_t> g_guest_swap_ns[kRing] = {};
std::atomic<uint64_t> g_cp_swap_ns[kRing] = {};
// Per frame (indexed by the swap that ends it): first and last kick, the
// CP's first execution, and how many kicks.
std::atomic<uint64_t> g_kick_first_ns[kRing] = {};
std::atomic<uint64_t> g_kick_last_ns[kRing] = {};
std::atomic<uint32_t> g_kicks[kRing] = {};
std::atomic<uint64_t> g_cp_start_ns[kRing] = {};
// CP WAIT_REG_MEM time per frame and its longest wait's address.
std::atomic<uint64_t> g_cp_wait_ns[kRing] = {};
std::atomic<uint32_t> g_cp_wait_addr[kRing] = {};
std::atomic<uint64_t> g_cp_wait_longest_ns[kRing] = {};
uint64_t g_cp_wait_total_last = 0;

// Earliest pad change not yet returned to the guest (0 = none).
std::atomic<uint64_t> g_input_ns{0};
std::atomic<uint32_t> g_last_read_packet{0};
// A read input waiting for the present of the frame swapped after the read.
std::atomic<uint64_t> g_pending_input_ns{0};
std::atomic<uint64_t> g_pending_read_ns{0};
std::atomic<uint64_t> g_pending_swap{0};

// Present thread only.
uint64_t g_last_presented = 0;
uint32_t g_presents = 0;
std::vector<uint32_t> g_swap_to_cp_us;
std::vector<uint32_t> g_cp_to_present_us;
std::vector<uint32_t> g_input_to_read_us;
std::vector<uint32_t> g_input_to_present_us;

uint32_t Us(uint64_t ns) {
  return uint32_t(std::min<uint64_t>(ns / 1000, UINT32_MAX));
}

void Percentiles(std::vector<uint32_t>& v, double& p50, double& p95) {
  if (v.empty()) {
    p50 = p95 = 0.0;
    return;
  }
  std::sort(v.begin(), v.end());
  p50 = v[v.size() / 2] / 1000.0;
  p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)] / 1000.0;
}

void OnCpSwap() {
  if (!cvars::xdroid_lat_meter) {
    return;
  }
  uint64_t swap = g_cp_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
  g_cp_swap_ns[swap % kRing].store(NowNs(), std::memory_order_release);
  g_cp_start_ns[(swap + 1) % kRing].store(0, std::memory_order_relaxed);
  xe::gpu::XdroidGpuHooks& hooks = xe::gpu::GetXdroidGpuHooks();
  uint64_t total = hooks.cp_wait_ns.load(std::memory_order_relaxed);
  g_cp_wait_ns[swap % kRing].store(total - g_cp_wait_total_last,
                                   std::memory_order_relaxed);
  g_cp_wait_total_last = total;
  g_cp_wait_addr[swap % kRing].store(
      hooks.cp_wait_longest_addr.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  g_cp_wait_longest_ns[swap % kRing].store(
      hooks.cp_wait_longest_ns.exchange(0, std::memory_order_relaxed),
      std::memory_order_relaxed);
}

double Ms(uint64_t ns, uint64_t base) {
  return ns && base ? (double(int64_t(ns - base)) / 1e6) : -1.0;
}

void OnPresent() {
  if (!cvars::xdroid_lat_meter) {
    return;
  }
  uint64_t swap = g_cp_swaps.load(std::memory_order_acquire);
  if (!swap || swap == g_last_presented) {
    return;
  }
  g_last_presented = swap;
  uint64_t now = NowNs();
  uint64_t cp_ns = g_cp_swap_ns[swap % kRing].load(std::memory_order_acquire);
  if (cp_ns && now >= cp_ns) {
    g_cp_to_present_us.push_back(Us(now - cp_ns));
  }
  if (g_guest_swaps.load(std::memory_order_acquire) >= swap) {
    uint64_t guest_ns =
        g_guest_swap_ns[swap % kRing].load(std::memory_order_acquire);
    if (guest_ns && cp_ns >= guest_ns) {
      g_swap_to_cp_us.push_back(Us(cp_ns - guest_ns));
    }
  }
  if (cvars::xdroid_lat_trace > 0 && swap > 1 &&
      g_guest_swaps.load(std::memory_order_acquire) >= swap) {
    --cvars::xdroid_lat_trace;
    uint64_t base =
        g_guest_swap_ns[(swap - 1) % kRing].load(std::memory_order_acquire);
    uint32_t i = swap % kRing;
    XELOGI(
        "XDROID-LATF {}: kicks {} first {:.1f} last {:.1f} swap {:.1f} | cp "
        "start {:.1f} swap {:.1f} waits {:.1f} (longest {:.1f} at {:08X}) | "
        "present {:.1f} ms",
        swap, g_kicks[i].load(std::memory_order_relaxed),
        Ms(g_kick_first_ns[i].load(std::memory_order_relaxed), base),
        Ms(g_kick_last_ns[i].load(std::memory_order_relaxed), base),
        Ms(g_guest_swap_ns[i].load(std::memory_order_relaxed), base),
        Ms(g_cp_start_ns[i].load(std::memory_order_relaxed), base),
        Ms(cp_ns, base),
        g_cp_wait_ns[i].load(std::memory_order_relaxed) / 1e6,
        g_cp_wait_longest_ns[i].load(std::memory_order_relaxed) / 1e6,
        g_cp_wait_addr[i].load(std::memory_order_relaxed), Ms(now, base));
  }
  uint64_t input_ns = g_pending_input_ns.load(std::memory_order_acquire);
  if (input_ns &&
      swap >= g_pending_swap.load(std::memory_order_relaxed)) {
    uint64_t read_ns = g_pending_read_ns.load(std::memory_order_relaxed);
    g_input_to_read_us.push_back(Us(read_ns - input_ns));
    g_input_to_present_us.push_back(Us(now - input_ns));
    g_pending_input_ns.store(0, std::memory_order_release);
  }
  if (++g_presents < kWindow) {
    return;
  }
  double sc50, sc95, cp50, cp95, ir50, ir95, ip50, ip95;
  Percentiles(g_swap_to_cp_us, sc50, sc95);
  Percentiles(g_cp_to_present_us, cp50, cp95);
  Percentiles(g_input_to_read_us, ir50, ir95);
  Percentiles(g_input_to_present_us, ip50, ip95);
  XELOGI(
      "XDROID-LAT: swap->cp p50 {:.1f} p95 {:.1f} ms | cp->present p50 {:.1f} "
      "p95 {:.1f} ms | inputs {} input->read p50 {:.1f} p95 {:.1f} ms "
      "input->present p50 {:.1f} p95 {:.1f} ms",
      sc50, sc95, cp50, cp95, g_input_to_present_us.size(), ir50, ir95, ip50,
      ip95);
  g_presents = 0;
  g_swap_to_cp_us.clear();
  g_cp_to_present_us.clear();
  g_input_to_read_us.clear();
  g_input_to_present_us.clear();
}

}  // namespace

void InstallLatencyMeter() {
  static std::once_flag installed;
  std::call_once(installed, [] {
    xe::gpu::GetXdroidGpuHooks().on_cp_swap.store(OnCpSwap,
                                                  std::memory_order_release);
    xe::ui::g_xdroid_on_present.store(OnPresent, std::memory_order_release);
  });
}

void LatNoteInputChange() {
  if (!cvars::xdroid_lat_meter) {
    return;
  }
  uint64_t expected = 0;
  g_input_ns.compare_exchange_strong(expected, NowNs(),
                                     std::memory_order_acq_rel);
}

void LatNoteInputRead(uint32_t packet) {
  if (!cvars::xdroid_lat_meter) {
    return;
  }
  if (g_last_read_packet.exchange(packet, std::memory_order_relaxed) ==
      packet) {
    return;
  }
  uint64_t input_ns = g_input_ns.exchange(0, std::memory_order_acq_rel);
  if (!input_ns || g_pending_input_ns.load(std::memory_order_acquire)) {
    return;
  }
  g_pending_read_ns.store(NowNs(), std::memory_order_relaxed);
  g_pending_swap.store(g_guest_swaps.load(std::memory_order_relaxed) + 1,
                       std::memory_order_relaxed);
  g_pending_input_ns.store(input_ns, std::memory_order_release);
}

void LatNoteGuestSwap() {
  if (!cvars::xdroid_lat_meter) {
    return;
  }
  uint64_t swap = g_guest_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
  g_guest_swap_ns[swap % kRing].store(NowNs(), std::memory_order_release);
  uint32_t next = (swap + 1) % kRing;
  g_kick_first_ns[next].store(0, std::memory_order_relaxed);
  g_kick_last_ns[next].store(0, std::memory_order_relaxed);
  g_kicks[next].store(0, std::memory_order_relaxed);
}

void LatNoteKick() {
  if (!cvars::xdroid_lat_meter) {
    return;
  }
  uint32_t i = (g_guest_swaps.load(std::memory_order_relaxed) + 1) % kRing;
  uint64_t now = NowNs();
  if (!g_kick_first_ns[i].load(std::memory_order_relaxed)) {
    g_kick_first_ns[i].store(now, std::memory_order_relaxed);
  }
  g_kick_last_ns[i].store(now, std::memory_order_relaxed);
  g_kicks[i].fetch_add(1, std::memory_order_relaxed);
}

void LatNoteCpExecute() {
  if (!cvars::xdroid_lat_meter) {
    return;
  }
  uint32_t i = (g_cp_swaps.load(std::memory_order_relaxed) + 1) % kRing;
  if (!g_cp_start_ns[i].load(std::memory_order_relaxed)) {
    g_cp_start_ns[i].store(NowNs(), std::memory_order_relaxed);
  }
}

}  // namespace xdroid
