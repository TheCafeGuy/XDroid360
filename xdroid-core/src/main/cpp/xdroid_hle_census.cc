// XDroid360 HLE census: counts every kernel export call (xboxkrnl, xam) per
// 10 s window and logs the top callers plus the guest clocks against the host
// clock. Built to tell a stopped game sim (mission 7 freeze, 2026-11-14) from
// a stalled guest clock or a game waiting on something: compare the windows
// before and during a freeze. Wraps each trampoline in a counting thunk before
// the title loads, so xex import resolution picks the thunks up; cost is one
// relaxed atomic add per HLE call.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/export_resolver.h"

DEFINE_bool(xdroid_hle_census, true,
            "Log HLE export call counts (top 16) and guest vs host clock "
            "deltas every 10 s (boot only).",
            "XDroid360");

namespace xdroid {
namespace {

constexpr size_t kSlots = 1024;
std::array<std::atomic<uint64_t>, kSlots> g_count{};
std::array<xe::cpu::ExportTrampoline, kSlots> g_orig{};
std::array<const char*, kSlots> g_name{};
size_t g_used = 0;

template <size_t I>
void Thunk(xe::cpu::ppc::PPCContext* ctx) {
  g_count[I].fetch_add(1, std::memory_order_relaxed);
  g_orig[I](ctx);
}

template <size_t... I>
constexpr std::array<xe::cpu::ExportTrampoline, kSlots> MakeThunks(
    std::index_sequence<I...>) {
  return {&Thunk<I>...};
}

void CensusLoop() {
  std::vector<uint64_t> last(kSlots, 0);
  uint64_t gu = xe::Clock::QueryGuestUptimeMillis();
  uint64_t gt = xe::Clock::QueryGuestTickCount();
  uint64_t gs = xe::Clock::QueryGuestSystemTime();
  uint64_t hu = xe::Clock::QueryHostUptimeMillis();
  const uint64_t freq = std::max<uint64_t>(1, xe::Clock::guest_tick_frequency());
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::vector<std::pair<uint64_t, size_t>> d;
    uint64_t total = 0;
    for (size_t i = 0; i < g_used; ++i) {
      uint64_t c = g_count[i].load(std::memory_order_relaxed);
      if (c != last[i]) d.emplace_back(c - last[i], i);
      total += c - last[i];
      last[i] = c;
    }
    std::sort(d.rbegin(), d.rend());
    std::string top;
    for (size_t k = 0; k < d.size() && k < 16; ++k) {
      top += fmt::format(" {}={}", g_name[d[k].second], d[k].first);
    }
    uint64_t gu2 = xe::Clock::QueryGuestUptimeMillis();
    uint64_t gt2 = xe::Clock::QueryGuestTickCount();
    uint64_t gs2 = xe::Clock::QueryGuestSystemTime();
    uint64_t hu2 = xe::Clock::QueryHostUptimeMillis();
    uint64_t host = hu2 - hu;
    uint64_t up = uint32_t(gu2 - gu);  // guest uptime wraps at 32 bits
    uint64_t tick = (gt2 - gt) * 1000 / freq;
    uint64_t sys = (gs2 - gs) / 10000;  // FILETIME 100 ns units
    // Flag a guest clock that drifts > 20% from the host over the window.
    auto off = [host](uint64_t g) { return g * 5 < host * 4 || g * 5 > host * 6; };
    XELOGI(
        "XDROID-HLE: {} s calls {} exports {} top:{} | clock ms host {} up {} "
        "tick {} sys {}{}",
        host / 1000, total, d.size(), top, host, up, tick, sys,
        (off(up) || off(tick) || off(sys)) ? " CLOCK-OFF" : "");
    gu = gu2, gt = gt2, gs = gs2, hu = hu2;
  }
}

}  // namespace

void InstallHleCensus(xe::cpu::ExportResolver* resolver) {
  if (!cvars::xdroid_hle_census || !resolver) return;
  static constexpr auto kThunks = MakeThunks(std::make_index_sequence<kSlots>{});
  size_t skipped = 0;
  for (xe::cpu::Export* e : resolver->all_exports_by_name()) {
    if (e->get_type() != xe::cpu::Export::Type::kFunction ||
        !e->function_data.trampoline) {
      continue;
    }
    if (g_used == kSlots) {
      ++skipped;
      continue;
    }
    g_orig[g_used] = e->function_data.trampoline;
    g_name[g_used] = e->name;
    e->function_data.trampoline = kThunks[g_used++];
  }
  XELOGI("XDROID-HLE: census on, {} exports wrapped, {} over the {} slots",
         g_used, skipped, kSlots);
  std::thread(CensusLoop).detach();
}

}  // namespace xdroid
