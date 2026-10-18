/**
 * XDroid360: Ace Combat 6 (4E4D07D1) frame-timing hooks.
 *
 * Two arms, both off by default and live (docs/design/ac6-60fps.md):
 *
 *  - ac6_unlock_probe (P60-0): measurement arm. Asks for a swap every vblank
 *    (60 FPS cap) and keeps game speed right, so a mission shows how far the
 *    phone is from 60. Not a product feature; it also stresses the CP lag
 *    behind HUD-1.
 *  - ac6_slowdown_guard (P60-1b): keeps the 30 FPS cap but lets the game run
 *    at true speed down to ac6_min_sim_fps instead of going slow-motion below
 *    30 (thermal throttling).
 *
 * Mechanism, from sal063/AC6_recomp (render_hooks.cpp, ac6_fps_physics_fix.cpp,
 * commits f30af372, d9828e08, 6dc41207, 175480ef, fc54d92a, c2e2fbfb; BSD
 * 3-Clause, Copyright (c) 2026, rexglue, license text in xdroid_ac6_fixes.cc):
 *  - 0x821EFF30, present-interval encoder: r10 = 1 (INTERVAL_ONE). Without
 *    their jump hook: the game's own compare chain handles 1.
 *  - 0x821EFD38: frame-delta divisor r29 60 -> 30, so the integer sim delta
 *    (100 = one 30 FPS frame) is ~50 at 60 FPS.
 *  - 0x821EFD74, after the game's min(floor(x)+1, 100): delta with a carried
 *    fractional remainder (no +1 bias), cap 3000/ac6_min_sim_fps, and dt-snap
 *    to the target when within ~1 ms.
 *  - Flight model: two accumulators ([fm+1320] force command in 0x823046A0,
 *    [fm+1456] trigger command in 0x82329B40) step by per-frame constants, and
 *    the sink drop (f9 at 0x82304F40) sits in a 1/dt feedback loop. All are
 *    scaled by this frame's delta / 100.
 *  - In-engine cutscenes (demo-manager Exec 0x82184460 / 0x821856F8) are
 *    frame-locked: the hooks stand down while they tick.
 *
 * Differences from the recomp:
 *  - "Gameplay active" comes from the flight-model step itself (entries of
 *    0x823046A0 / 0x82305278, which halt in menus and pause), not from a GPU
 *    shader-hash stamp, so the CP is not involved.
 *  - The physics ratio is the delta the game actually got this frame (one
 *    source of truth), not the host present timer.
 *  - The accumulators are scaled at their store instructions, found at
 *    install time by scanning each function for stfs/stfsu to that offset,
 *    instead of wrapping the function (SetGuestHook has no exit kind).
 *  - Every fixed address is checked before anything is hooked; the caller
 *    checks the effect manager vtables (same executable as the recomp).
 */

#include "xdroid_ac6_timing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/memory.h"

DEFINE_bool(ac6_timing_hooks, false,
            "Ace Combat 6: install the frame-timing hooks used by "
            "ac6_unlock_probe and ac6_slowdown_guard (boot only; inert "
            "while both are off).",
            "XDroid360");
DEFINE_bool(ac6_unlock_probe, false,
            "Ace Combat 6 (needs ac6_timing_hooks): measurement arm, 60 FPS "
            "cap in flight with game speed kept right. Not for play. Live.",
            "XDroid360");
// OEM-1: the phone interpolates frames itself (src/xenia/ui/presenter.cc).
DECLARE_string(present_oem_postprocess);
DEFINE_bool(ac6_slowdown_guard, false,
            "Ace Combat 6 (needs ac6_timing_hooks): keep true game speed "
            "below 30 FPS (down to ac6_min_sim_fps) instead of slow motion. "
            "Live.",
            "XDroid360");
DEFINE_int32(ac6_min_sim_fps, 20,
             "Ace Combat 6: lowest FPS that still runs at true speed under "
             "the timing hooks (10-30; stock is 30). Live.",
             "XDroid360");
DEFINE_bool(ac6_dt_snap, true,
            "Ace Combat 6: under the timing hooks, feed the exact target "
            "step when the frame time is within ~1 ms of it. Live.",
            "XDroid360");

namespace xdroid {
namespace {

constexpr uint32_t kDeltaDivisorSite = 0x821EFD38;   // r29 = divisor
constexpr uint32_t kDeltaFinalSite = 0x821EFD74;     // r8 = final delta
constexpr uint32_t kPresentIntervalSite = 0x821EFF30;  // r10 = interval
constexpr uint32_t kPathDropSite = 0x82304F40;       // fsubs f11,f11,f9
constexpr uint32_t kPathDropInsn = 0xED6B4828;
constexpr uint32_t kForceStep = 0x823046A0;
constexpr uint32_t kForceStepEnd = 0x82305278;       // next function
constexpr uint32_t kGroundStep = 0x82305278;
constexpr uint32_t kInputShaping = 0x82329B40;
constexpr uint32_t kInputShapingEnd = 0x82329F40;    // next function
constexpr uint32_t kCinematicDD = 0x82184460;
constexpr uint32_t kCinematicEM = 0x821856F8;
constexpr int16_t kForceCommandOffset = 1320;
constexpr int16_t kTriggerCommandOffset = 1456;

// Game delta units: 100 = one 30 FPS frame (33.3 ms).
constexpr double kNativeDelta = 100.0;
constexpr int64_t kFlightDecayUs = 100000;
constexpr int64_t kCinematicDecayUs = 100000;
constexpr int64_t kLogPeriodUs = 10000000;

struct StoreSite {
  uint32_t address = 0;
  uint32_t fs = 0;  // FPR stored
  uint32_t ra = 0;  // base GPR
  int16_t offset = 0;
};

struct State {
  uint8_t* membase = nullptr;
  std::vector<StoreSite> store_sites;
  std::atomic<int64_t> last_flight_us{INT64_MIN};
  std::atomic<int64_t> last_cinematic_us{INT64_MIN};
  // This frame's delta / 100 while the hooks are active, else exactly 1.
  std::atomic<double> step_ratio{1.0};
  // Guest main thread only (the delta sites run once per frame there).
  bool divisor_remapped = false;
  double remainder = 0.0;
  int64_t last_log_us = 0;
  // Window counters for the periodic log line.
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> active_frames{0};
  std::atomic<uint64_t> snaps{0};
  std::atomic<uint64_t> delta_sum{0};
  std::atomic<uint64_t> interval_forced{0};
  std::atomic<uint64_t> stores_scaled{0};
  std::atomic<uint64_t> drops_scaled{0};
} g_state;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool Fresh(const std::atomic<int64_t>& stamp, int64_t decay_us) {
  const int64_t last = stamp.load(std::memory_order_relaxed);
  return last != INT64_MIN && NowUs() - last <= decay_us;
}

// OEM-1: a phone frame interpolator doubles a steady 30 FPS; an unlocked,
// varying frame rate breaks its cadence, so the unlock yields to it. The
// slowdown guard (below 30) stays available.
bool UnlockProbeOn() {
  if (!cvars::ac6_unlock_probe) {
    return false;
  }
  const bool oem_interpolates =
      cvars::present_oem_postprocess == "interpolate" ||
      cvars::present_oem_postprocess == "both";
  static std::atomic<bool> warned{false};
  if (oem_interpolates && !warned.exchange(true, std::memory_order_relaxed)) {
    XELOGW(
        "XDROID-AC6T: ac6_unlock_probe refused while present_oem_postprocess "
        "says the phone interpolates frames");
  }
  return !oem_interpolates;
}

bool TimingActive() {
  return (UnlockProbeOn() || cvars::ac6_slowdown_guard) &&
         Fresh(g_state.last_flight_us, kFlightDecayUs) &&
         !Fresh(g_state.last_cinematic_us, kCinematicDecayUs);
}

double MaxDelta() {
  const int32_t min_fps = std::clamp(cvars::ac6_min_sim_fps, 10, 30);
  return 3000.0 / double(min_fps);
}

void FlightStepEntry(xe::cpu::ppc::PPCContext*, void*, void*) {
  g_state.last_flight_us.store(NowUs(), std::memory_order_relaxed);
}

void CinematicEntry(xe::cpu::ppc::PPCContext*, void*, void*) {
  g_state.last_cinematic_us.store(NowUs(), std::memory_order_relaxed);
}

void PresentIntervalHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  if (!UnlockProbeOn() || !TimingActive()) {
    return;
  }
  if (uint32_t(ctx->r[10]) != 1) {
    ctx->r[10] = 1;
    g_state.interval_forced.fetch_add(1, std::memory_order_relaxed);
  }
}

void DeltaDivisorHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  // Decided once per frame here; DeltaFinalHook follows this decision so the
  // two sites cannot disagree when a decay window ends between them.
  g_state.divisor_remapped = TimingActive();
  if (g_state.divisor_remapped) {
    ctx->r[29] = 30;
  }
}

void MaybeLog() {
  const int64_t now = NowUs();
  if (now - g_state.last_log_us < kLogPeriodUs) {
    return;
  }
  g_state.last_log_us = now;
  const uint64_t frames = g_state.frames.exchange(0);
  const uint64_t active = g_state.active_frames.exchange(0);
  const uint64_t delta_sum = g_state.delta_sum.exchange(0);
  XELOGI(
      "XDROID-AC6T probe {} guard {} frames {} active {} mean_delta {:.1f} "
      "snaps {} interval1 {} stores_scaled {} drops_scaled {}",
      UnlockProbeOn() ? 1 : 0, cvars::ac6_slowdown_guard ? 1 : 0,
      frames, active, active ? double(delta_sum) / double(active) : 0.0,
      g_state.snaps.exchange(0), g_state.interval_forced.exchange(0),
      g_state.stores_scaled.exchange(0), g_state.drops_scaled.exchange(0));
}

// Right after the game's r8 = min(floor(r30 * 100 / r10) + 1, 100), with
// r30 = elapsed timebase ticks, r10 = ticks per frame (freq / r29).
void DeltaFinalHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  g_state.frames.fetch_add(1, std::memory_order_relaxed);
  MaybeLog();
  const uint32_t divisor = uint32_t(ctx->r[29]);
  const uint32_t ticks_per_frame = uint32_t(ctx->r[10]);
  const bool remapped = g_state.divisor_remapped;
  g_state.divisor_remapped = false;
  if (!remapped || divisor != 30 || !ticks_per_frame) {
    g_state.remainder = 0.0;
    g_state.step_ratio.store(1.0, std::memory_order_relaxed);
    return;
  }
  const double max_delta = MaxDelta();
  double exact =
      double(uint32_t(ctx->r[30])) * kNativeDelta / double(ticks_per_frame);
  // Time slower than ac6_min_sim_fps is dropped, not banked.
  exact = std::min(exact, max_delta);
  double delta;
  const double target_delta = UnlockProbeOn() ? 50.0 : kNativeDelta;
  // ~1 ms window (3 units of 33.3 ms / 100).
  if (cvars::ac6_dt_snap && target_delta <= max_delta &&
      std::fabs(exact - target_delta) < 3.0) {
    delta = target_delta;
    g_state.remainder = 0.0;
    g_state.snaps.fetch_add(1, std::memory_order_relaxed);
  } else {
    const double base = g_state.remainder + exact;
    delta = std::clamp(std::floor(base), 1.0, max_delta);
    g_state.remainder = base - delta;
  }
  ctx->r[8] = uint64_t(delta);
  g_state.step_ratio.store(delta / kNativeDelta, std::memory_order_relaxed);
  g_state.active_frames.fetch_add(1, std::memory_order_relaxed);
  g_state.delta_sum.fetch_add(uint64_t(delta), std::memory_order_relaxed);
}

// Before the fsubs that applies the sink drop f9 inside the force step.
void PathDropHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  const double ratio = g_state.step_ratio.load(std::memory_order_relaxed);
  if (ratio != 1.0) {
    ctx->f[9] *= ratio;
    g_state.drops_scaled.fetch_add(1, std::memory_order_relaxed);
  }
}

// Before an stfs/stfsu of a per-frame-stepped accumulator: scale the change
// it is about to store, new = old + (new - old) * ratio. Several stores per
// call compose to the same scaled net change.
void StoreScaleHook(xe::cpu::ppc::PPCContext* ctx, void* arg0, void*) {
  const double ratio = g_state.step_ratio.load(std::memory_order_relaxed);
  if (ratio == 1.0) {
    return;
  }
  const auto& site = *static_cast<const StoreSite*>(arg0);
  const uint32_t ea = uint32_t(ctx->r[site.ra]) + uint32_t(int32_t(site.offset));
  if (ea < 0x10000u) {
    return;  // null-page store: let the guest fault as it would
  }
  const float old_value = xe::load_and_swap<float>(g_state.membase + ea);
  const float new_value = float(ctx->f[site.fs]);
  if (new_value == old_value || !std::isfinite(old_value) ||
      !std::isfinite(new_value)) {
    return;
  }
  ctx->f[site.fs] =
      double(float(double(old_value) +
                   (double(new_value) - double(old_value)) * ratio));
  g_state.stores_scaled.fetch_add(1, std::memory_order_relaxed);
}

void FindStoreSites(const uint8_t* membase, uint32_t start, uint32_t end,
                    int16_t offset, std::vector<StoreSite>& out) {
  for (uint32_t address = start; address < end; address += 4) {
    const uint32_t insn = xe::load_and_swap<uint32_t>(membase + address);
    const uint32_t opcode = insn >> 26;
    const uint32_t ra = (insn >> 16) & 31;
    // stfs (52) / stfsu (53) frS, d(rA); rA = 0 means an absolute address.
    if ((opcode == 52 || opcode == 53) && ra != 0 &&
        int16_t(insn & 0xFFFF) == offset) {
      StoreSite site;
      site.address = address;
      site.fs = (insn >> 21) & 31;
      site.ra = ra;
      site.offset = offset;
      out.push_back(site);
    }
  }
}

}  // namespace

bool InstallAc6Timing(xe::Emulator* emulator) {
  if (!cvars::ac6_timing_hooks) {
    return false;
  }
  uint8_t* membase = emulator->memory()->virtual_membase();
  auto word = [membase](uint32_t address) {
    return xe::load_and_swap<uint32_t>(membase + address);
  };
  const uint32_t interval_insn = word(kPresentIntervalSite);
  const uint32_t interval_op = interval_insn >> 26;
  // cmpwi/cmplwi (11/10) or cmpw/cmplw (31) reading r10.
  const bool interval_ok = (interval_op == 10 || interval_op == 11 ||
                            interval_op == 31) &&
                           ((interval_insn >> 16) & 31) == 10;
  XELOGI(
      "XDroid360: AC6 timing sites {:08X}={:08X} {:08X}={:08X} "
      "{:08X}={:08X} {:08X}={:08X}",
      kDeltaDivisorSite, word(kDeltaDivisorSite), kDeltaFinalSite,
      word(kDeltaFinalSite), kPresentIntervalSite, interval_insn,
      kPathDropSite, word(kPathDropSite));
  if (word(kPathDropSite) != kPathDropInsn || !interval_ok ||
      !word(kDeltaDivisorSite) || !word(kDeltaFinalSite)) {
    XELOGW(
        "XDroid360: AC6 timing hooks not installed - executable layout does "
        "not match");
    return false;
  }
  g_state.membase = membase;
  g_state.store_sites.clear();
  FindStoreSites(membase, kForceStep, kForceStepEnd, kForceCommandOffset,
                 g_state.store_sites);
  const size_t force_sites = g_state.store_sites.size();
  FindStoreSites(membase, kInputShaping, kInputShapingEnd,
                 kTriggerCommandOffset, g_state.store_sites);
  const size_t trigger_sites = g_state.store_sites.size() - force_sites;
  if (!force_sites || !trigger_sites) {
    // The delta, interval and sink hooks still apply; only the two
    // accumulators would run per frame (throttle response off by the ratio).
    XELOGW(
        "XDroid360: AC6 timing: accumulator stores not found (force {}, "
        "trigger {})",
        force_sites, trigger_sites);
  }

  using Kind = xe::cpu::Processor::GuestHookKind;
  xe::cpu::Processor* processor = emulator->processor();
  auto hook = [processor](uint32_t address, const char* name,
                          xe::cpu::BuiltinFunction::Handler handler, void* arg0,
                          Kind kind) {
    processor->SetGuestHook(
        address, processor->DefineBuiltin(name, handler, arg0, nullptr), kind);
  };
  hook(kForceStep, "XDroidAc6FlightStep", FlightStepEntry, nullptr,
       Kind::kEntry);
  hook(kGroundStep, "XDroidAc6GroundStep", FlightStepEntry, nullptr,
       Kind::kEntry);
  hook(kCinematicDD, "XDroidAc6CinematicDD", CinematicEntry, nullptr,
       Kind::kEntry);
  hook(kCinematicEM, "XDroidAc6CinematicEM", CinematicEntry, nullptr,
       Kind::kEntry);
  hook(kDeltaDivisorSite, "XDroidAc6DeltaDivisor", DeltaDivisorHook, nullptr,
       Kind::kInstruction);
  hook(kDeltaFinalSite, "XDroidAc6DeltaFinal", DeltaFinalHook, nullptr,
       Kind::kInstruction);
  hook(kPresentIntervalSite, "XDroidAc6PresentInterval", PresentIntervalHook,
       nullptr, Kind::kInstruction);
  hook(kPathDropSite, "XDroidAc6PathDrop", PathDropHook, nullptr,
       Kind::kInstruction);
  for (StoreSite& site : g_state.store_sites) {
    hook(site.address, "XDroidAc6AccumulatorStore", StoreScaleHook, &site,
         Kind::kInstruction);
    XELOGI("XDroid360: AC6 timing: accumulator store {:08X} f{} {}(r{})",
           site.address, site.fs, site.offset, site.ra);
  }
  g_state.last_log_us = NowUs();
  XELOGI(
      "XDroid360: AC6 timing hooks installed (probe {}, guard {}, "
      "{} accumulator stores)",
      cvars::ac6_unlock_probe ? 1 : 0, cvars::ac6_slowdown_guard ? 1 : 0,
      g_state.store_sites.size());
  return true;
}

}  // namespace xdroid
