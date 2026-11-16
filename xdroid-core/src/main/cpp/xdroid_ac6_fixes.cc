/**
 * XDroid360: Ace Combat 6 (4E4D07D1) title-specific fixes.
 *
 * Effect mode race ("one-frame afterburner / light sprite / cloud flicker"),
 * ported from sal063/AC6_recomp src/ac6_backend_fixes/ac6_effect_mode_fix.cpp
 * (BSD 3-Clause, Copyright (c) 2026, rexglue). Analysis by the AC6_recomp
 * authors: the effect UPDATE and DRAW jobs are submitted to the game's second
 * worker pool through the same dispatcher (effect manager vtbl[+0x04] =
 * 0x822B3248) and are told apart only by a shared mode word at
 * [manager + 0xA1E84]. On the console fixed core assignment kept the two jobs
 * from overlapping; under emulation a host stall lets the second submit
 * overwrite the word before the first job reads it, both jobs draw, the update
 * is skipped and the effects are emitted twice for a frame. The fix snapshots
 * the mode when the job is submitted (pool Submit 0x821D4A78) and dispatches
 * from the snapshot.
 *
 * Here the snapshot is taken by an entry hook on Submit and the dispatcher is
 * replaced with a native one; see Processor::SetGuestHook.
 *
 * Cloud/effect de-swizzle (CLOUD-1, AC6_recomp 313fd236): two AC6 pixel
 * shaders (a restore pass and the cloud compositor's scene fetch, slot 4)
 * undo the raw EDRAM sub-tile order of their source by hand. The texture cache
 * hands them linear texels, so that permutation scrambles clouds and effects;
 * they sample the texel under the pixel instead (SpirvShaderTranslator,
 * deswizzle_identity_ps_hashes).
 *
 * Portions of this file are derived from AC6_recomp
 * (https://github.com/sal063/AC6_recomp), distributed under the following
 * license:
 *
 * BSD 3-Clause License
 *
 * Copyright (c) 2026, rexglue
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "xdroid_ac6_fixes.h"

#include <atomic>
#include <chrono>
#include <arm_neon.h>

#include <cstring>
#include <vector>
#include <iterator>
#include <cmath>
#include <mutex>

#include "xdroid_ac6_jobs.h"
#include "xdroid_ac6_timing.h"

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/wake_generation.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DEFINE_bool(ac6_effect_mode_fix, true,
            "Ace Combat 6: fix the one-frame afterburner/effect/cloud flicker "
            "by dispatching effect jobs with the mode they were submitted "
            "with (port of the AC6_recomp fix).",
            "XDroid360");

DEFINE_bool(ac6_cloud_deswizzle_fix, true,
            "Ace Combat 6: fix clouds and effects drawn with scrambled content "
            "by sampling linearly where the game de-swizzles EDRAM tiles by "
            "hand (port of the AC6_recomp fix).",
            "XDroid360");

DEFINE_int32(ac6_native_audio, 1,
             "Ace Combat 6 audio: the hottest XAudio2 mixer loops run as native "
             "code with the same operations and rounding (the biquad cascade "
             "0x82379DD0, the ramped mix 0x823A73F8 and the 16-bit mono "
             "resampler 0x823BA0A0: most of the mixer thread's CPU in the "
             "sampler). 0 = guest code, 1 = native, 2 = "
             "guest code plus a check of the native result against it "
             "(XDROID-NATIVE lines). Restart.",
             "XDroid360");
DEFINE_bool(ac6_hud_slot_join, true,
            "Ace Combat 6 HUD-1 fix (temp, under test): the render thread "
            "picks the HUD vertex set (0x82138048) only after the frame "
            "counter it uses (0x826E4DD0) changed since the last pick. It "
            "waits for the counter itself to change, parked (no spin) and "
            "woken by the main loop's counter store, at most "
            "ac6_hud_slot_join_us; after a timeout it does not wait again "
            "until the counter moves. Live.",
            "XDroid360");
DEFINE_bool(ac6_resource_miss_log, true,
            "Ace Combat 6 forensic (HANGAR-1, temp): log resource handles the "
            "game fails to resolve (0x8233E958 returning null; the hangar "
            "crash at 0x82364158 dereferences that null) with their caller, "
            "and how long until the same handle resolves.",
            "XDroid360");
DEFINE_int32(ac6_hud_slot_join_us, 8000,
             "Longest wait of ac6_hud_slot_join per HUD set pick. Live. Arms "
             "J8/J20/J34 under test (HUD-H1).",
             "XDroid360");

namespace xdroid {
namespace {

constexpr uint32_t kAc6TitleId = 0x4E4D07D1;
// Pixel shader ucode hashes from AC6_recomp 313fd236 (found in our AC6 shader
// storage too): a restore pass (all fetches) and the cloud compositor, whose
// scene fetch (slot 4) de-swizzles while its mask/cloud fetches do not.
constexpr char kAc6DeswizzleHashes[] = "7d22894002d16018, 17e5e4ac3e713245:4";
constexpr uint32_t kPoolSubmit = 0x821D4A78;      // Submit(tag, thr, job, arg)
constexpr uint32_t kEffDispatcher = 0x822B3248;   // manager vtbl[+0x04]
constexpr uint32_t kEffUpdate0 = 0x822B2E20;      // mode 0, f1 = dt
constexpr uint32_t kEffUpdate1 = 0x822B0848;      // mode 1, f1 = dt
constexpr uint32_t kEffDrawX360 = 0x820B25D0;     // CX360EffectManager draw
constexpr uint32_t kEffDrawAce6 = 0x822B0AD8;     // CAce6EffectManager draw
constexpr uint32_t kVtblX360 = 0x8205829C;        // CX360EffectManager vtable
constexpr uint32_t kVtblAce6 = 0x82057BDC;        // CAce6EffectManager vtable
constexpr uint32_t kEffModeOffset = 0xA1E84;

uint32_t Load32(const uint8_t* membase, uint32_t address) {
  return xe::load_and_swap<uint32_t>(membase + address);
}

// HUD-1 (docs/runs/2026-10-10/gfx-hunt.md, hud1-s): the HUD draws use vertex
// set (counter + 2) % 3 (0x82138048 at 0x82138078); the main loop advances
// the counter once per iteration (0x821D7ECC). A pick that sees the same
// counter as the previous pick reuses the previous frame's set, which the
// game is already refilling: the HUD-1 garbage.
constexpr uint32_t kHudSetPickLoad = 0x82138078;   // lwz r11, 4(r11)
constexpr uint32_t kHudCounterStore = 0x821D7ECC;  // stw r11, 4(r31)
constexpr uint32_t kHudCounter = 0x826E4DD0;
constexpr uint32_t kHudSetPickLoadWord = 0x816B0004;
constexpr uint32_t kHudCounterStoreWord = 0x917F0004;

struct HudJoin {
  // Set by the render thread while its pick waits; the main loop's store hook
  // consumes it (exchange to false) and wakes the pick. Consuming it there
  // means a render thread that exits while parked (ExitIfTerminated never
  // returns, so no destructor or RAII guard runs) costs at most one extra
  // Bump, not one per main iteration for the rest of the session.
  std::atomic<bool> waiting{false};
  // Guest cpu of the main loop at its last wake (diagnostic only).
  std::atomic<uint8_t> main_cpu{0xFF};
  // Render thread only from here on.
  bool gave_up = false;
  uint32_t last_pick_counter = UINT32_MAX;
  uint64_t picks = 0, repeats = 0, waits = 0, timeouts = 0, waited_us = 0;
  uint64_t timeout_logs = 0;
  uint64_t last_report_us = 0;
} g_hud;

// The current guest thread's pinned guest cpu, 0xFF when not a guest thread.
uint8_t HudGuestCpu() {
  xe::kernel::XThread* thread = xe::kernel::XThread::GetCurrentThread();
  return thread ? thread->guest_object<xe::kernel::X_KTHREAD>()->current_cpu
                : uint8_t(0xFF);
}

uint64_t HudNowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// HANGAR-1 (docs/runs/2026-10-12/u2.log): the hangar crashed at 0x82364158
// on lwz r11, 0(r3) with r3 = 0, the result of the resource resolve
// 0x8233E958(handle) a few instructions earlier. The resolve returns null
// when the lookup in the table at 0x828C8080 fails (0x8233E994). Log the
// handle id and caller on a miss, and the delay until it resolves: a load
// still in flight (latency race) or an id that never comes back (unloaded).
constexpr uint32_t kResResolveMiss = 0x8233E994;  // li r3, 0
constexpr uint32_t kResResolveHit = 0x8233E9B0;   // lhz r11, 0xa(r31)
constexpr uint32_t kResResolveMissWord = 0x38600000;
constexpr uint32_t kResResolveHitWord = 0xA17F000A;

struct ResMiss {
  uint32_t id;
  uint64_t us;
};
std::atomic<uint32_t> g_res_miss_logged{0};
std::atomic<uint64_t> g_res_misses{0};
ResMiss g_res_pending[16];
std::atomic<uint32_t> g_res_pending_next{0};

void ResMissHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  if (!cvars::ac6_resource_miss_log) {
    return;
  }
  const uint8_t* membase = ctx->virtual_membase;
  const uint32_t handle = uint32_t(ctx->r[31]);
  const uint32_t id = Load32(membase, handle);
  // The caller's return address: saved at old r1 - 8 = r1 + 0x68.
  const uint32_t caller = Load32(membase, uint32_t(ctx->r[1]) + 0x68);
  g_res_misses.fetch_add(1, std::memory_order_relaxed);
  const uint32_t slot =
      g_res_pending_next.fetch_add(1, std::memory_order_relaxed) % 16;
  g_res_pending[slot] = {id, HudNowUs()};
  if (g_res_miss_logged.fetch_add(1, std::memory_order_relaxed) < 200) {
    XELOGW(
        "XDROID-RESMISS: resource {:08X} (handle {:08X}) not resolved for "
        "caller {:08X} on guest thread {:X}",
        id, handle, caller, ctx->thread_id);
  }
}

void ResHitHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  if (!cvars::ac6_resource_miss_log ||
      !g_res_misses.load(std::memory_order_relaxed)) {
    return;
  }
  const uint32_t id = Load32(ctx->virtual_membase, uint32_t(ctx->r[31]));
  for (ResMiss& miss : g_res_pending) {
    if (miss.us && miss.id == id) {
      XELOGI("XDROID-RESMISS: resource {:08X} resolved {} ms after its miss",
             id, (HudNowUs() - miss.us) / 1000);
      miss.us = 0;
    }
  }
}

// Main loop thread, before `stw r11, 4(r31)` (kHudCounterStoreWord) at
// 0x821D7ECC. We store the same value one instruction early, so a pick we
// release never loads the old counter (HUD-H1 verifier defect a); the guest
// stw then rewrites it. Keep this hook even though the pick polls the counter
// itself: the early store is the store half of the Dekker pair below (store
// counter, fence, load waiting), which is what lets the targeted wake reach a
// parked pick. Without it the pick only re-polls on the 1 ms backstop
// (kGpuWritebackBackstopMs) or on unrelated GPU write-backs.
//
// Order is load-bearing: store, seq_cst fence, waiting load, Bump. The pick
// does the mirror (waiting store, fence, generation load, counter load).
// Swapping either pair turns the fast handoff into a wait of up to 1 ms; it
// never deadlocks.
//
// The counter is guest memory a guest thread polls, the generation's stated
// contract (wake_generation.h). Bump -> WakeGpuWritebackWaiters takes the
// scheduler lock_ here, on the main fiber in user code: not a watchdog region
// (rule 8), and lock_ is never held across a fiber switch. It also sets
// preempt_requested on every runner of a CPU with a write-back waiter,
// including main itself when it shares the parked render's CPU; main then
// yields at its next safepoint, which is the intended handoff. The bump also
// counts as a GPU release in the spin-site statistics (a64_seq_memory.cc,
// guest_spin_park modes 3/4), about 20 per flight: negligible.
void HudCounterStoreHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  if (uint32_t(ctx->r[31]) + 4 != kHudCounter) {
    return;
  }
  xe::store_and_swap<uint32_t>(ctx->virtual_membase + kHudCounter,
                               uint32_t(ctx->r[11]));
  std::atomic_thread_fence(std::memory_order_seq_cst);
  g_hud.main_cpu.store(HudGuestCpu(), std::memory_order_relaxed);
  if (g_hud.waiting.load(std::memory_order_relaxed) &&
      g_hud.waiting.exchange(false, std::memory_order_relaxed)) {
    xe::threading::GuestGpuWritebackGeneration().Bump();
  }
}

// Render thread, before the load of the counter in the HUD set pick. Only
// this thread touches the non-atomic fields.
void HudSetPickHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  const uint8_t* membase = ctx->virtual_membase;
  const uint64_t now = HudNowUs();
  ++g_hud.picks;
  uint32_t counter = Load32(membase, kHudCounter);
  if (counter != g_hud.last_pick_counter) {
    g_hud.gave_up = false;
  } else {
    // Same counter as the last pick: this pick would reuse its set. Unlike
    // the old advance count, this also waits when main rewrote the counter
    // with an unchanged value (if it ever does, e.g. paused); the gave_up
    // latch bounds that to one cap wait per stall.
    ++g_hud.repeats;
    if (cvars::ac6_hud_slot_join && !g_hud.gave_up) {
      ++g_hud.waits;
      const uint64_t deadline =
          now + uint64_t(std::max(cvars::ac6_hud_slot_join_us, 0));
      auto& gen = xe::threading::GuestGpuWritebackGeneration();
      g_hud.waiting.store(true, std::memory_order_seq_cst);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      for (;;) {
        // |seen| before the counter (keep this order): either this load sees
        // main's early store, or main sees |waiting| and bumps past |seen|,
        // so the park returns at once and we re-poll (ParkOnGpuWriteback
        // re-checks the generation under lock_).
        const uint32_t seen = gen.Load();
        counter = Load32(membase, kHudCounter);
        if (counter != g_hud.last_pick_counter || HudNowUs() >= deadline) {
          break;
        }
        // XR-3 park: woken by main's Bump, by any CP write-back (each one
        // re-polls us) or by the 1 ms backstop. SpinYield only when the park
        // is declined.
        if (!xe::cpu::backend::spin_park_handler ||
            !xe::cpu::backend::spin_park_handler(seen)) {
          xe::kernel::GuestScheduler::SpinYield();
        }
      }
      g_hud.waiting.store(false, std::memory_order_relaxed);
      const uint64_t waited = HudNowUs() - now;
      g_hud.waited_us += waited;
      if (counter == g_hud.last_pick_counter) {
        // Main stalled (loading, pause, slow frame): skip further waits until
        // the counter moves, so one stall costs one cap wait, not one per
        // render frame.
        ++g_hud.timeouts;
        g_hud.gave_up = true;
        if (g_hud.timeout_logs++ < 200) {
          // Equal guest cpus mean main and render share a dispatch CPU, so
          // write-back preempts bounced main during the park.
          XELOGW(
              "XDROID-HUDJOIN: timeout cp frame {} guest swaps {} waited {} "
              "us counter {:08X} defer hold {} guest cpu render {} main {}",
              xe::logging::GetFrameNumber(), Ac6GuestSwaps(), waited, counter,
              Ac6CpDeferHolding() ? 1 : 0, unsigned(HudGuestCpu()),
              unsigned(g_hud.main_cpu.load(std::memory_order_relaxed)));
        }
      }
    }
  }
  g_hud.last_pick_counter = counter;
  if (now - g_hud.last_report_us >= 10'000'000) {
    g_hud.last_report_us = now;
    XELOGI(
        "XDROID-HUDSET: picks {} repeats {} (stale set without a join); "
        "join waits {} timeouts {} waited {} ms (totals)",
        g_hud.picks, g_hud.repeats, g_hud.waits, g_hud.timeouts,
        g_hud.waited_us / 1000);
  }
}

}  // namespace

bool Ac6HudJoinWaiting() {
  return g_hud.waiting.load(std::memory_order_relaxed);
}

namespace {

struct Entry {
  uint32_t job = 0;
  uint32_t arg = 0;
  uint32_t mode = 0;
  bool used = true;
};

struct State {
  std::mutex mutex;
  Entry entries[16];
  int next = 0;
  uint64_t corrections = 0;
} g_state;

bool IsEffectManagerJob(const uint8_t* membase, uint32_t job) {
  if (job < 0x82000000u || job >= 0xC0000000u) {
    return false;
  }
  const uint32_t vt = Load32(membase, job);
  if (vt < 0x82000000u || vt >= 0x82A00000u) {
    return false;
  }
  return Load32(membase, vt + 4) == kEffDispatcher;
}

// Entry hook on the pool Submit: remember the mode this job was queued with.
void SubmitEntryHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  Ac6JobsOnSubmit(ctx);
  if (!cvars::ac6_effect_mode_fix) {
    return;
  }
  const uint8_t* membase = ctx->virtual_membase;
  const uint32_t job = uint32_t(ctx->r[5]);
  if (!IsEffectManagerJob(membase, job)) {
    return;
  }
  const uint32_t mode = Load32(membase, job + kEffModeOffset);
  std::lock_guard<std::mutex> lock(g_state.mutex);
  Entry& slot = g_state.entries[g_state.next];
  slot.job = job;
  slot.arg = uint32_t(ctx->r[6]);
  slot.mode = mode;
  slot.used = false;
  g_state.next = (g_state.next + 1) & 15;
}

// Replacement for the dispatcher. Same dispatch as the guest code, but driven
// by the submit-time snapshot when there is one.
void DispatcherReplacement(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  const uint8_t* membase = ctx->virtual_membase;
  const uint32_t self = uint32_t(ctx->r[3]);
  const uint32_t self_arg = uint32_t(ctx->r[4]);
  uint32_t mode = Load32(membase, self + kEffModeOffset);
  {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    bool have = false;
    for (int i = 0; i < 16 && !have; ++i) {
      Entry& slot = g_state.entries[(g_state.next - 1 - i) & 15];
      if (!slot.used && slot.job == self && slot.arg == self_arg) {
        if (slot.mode != mode) {
          if (++g_state.corrections <= 8 ||
              (g_state.corrections & 0xFF) == 0) {
            XELOGI(
                "XDroid360: AC6 effect mode race corrected ({} -> {}), {} so "
                "far",
                mode, slot.mode, g_state.corrections);
          }
        }
        mode = slot.mode;
        slot.used = true;
        have = true;
      }
    }
  }
  xe::cpu::Processor* processor = ctx->processor;
  if (mode == 2) {
    const uint32_t vt = Load32(membase, self);
    const uint32_t draw = (vt >= 0x82000000u && vt < 0x82A00000u)
                              ? Load32(membase, vt + 0x100)
                              : 0u;
    if (draw) {
      processor->Execute(ctx->thread_state, draw);  // r3 = this, r4 = arg
    }
    return;
  }
  if (mode == 0 || mode == 1) {
    float dt;
    const uint32_t dt_bits = uint32_t(ctx->r[4]);
    std::memcpy(&dt, &dt_bits, sizeof(dt));
    ctx->f[1] = double(dt);
    processor->Execute(ctx->thread_state, mode == 0 ? kEffUpdate0
                                                     : kEffUpdate1);
  }
  // mode >= 3: the guest dispatcher returns without dispatching.
}

// XAudio2 biquad cascade, guest 0x82379DD0(state1, state2, 2 * sections,
// coefficients, source, samples, destination): copies the samples to the
// destination, then runs each section over them in place. Direct form II with
// its two state words in memory; per sample
//   w = x - s1 * a1 - s2 * a2,  y = w * b0 + s1 * b1 + s2 * b2,  s2 = s1, s1 = w
// in exactly the guest's operation order (fmuls, fnmsubs, fmadds, fnmsubs,
// fmadds). The translated loop pays double conversions, denormal checks and
// FPSCR updates on each of those; here the state stays in registers and each
// step is one host instruction with the same rounding as the console's.
constexpr uint32_t kBiquad = 0x82379DD0;
constexpr uint32_t kBiquadExit = 0x82379E84;  // li r3, 0 (every path)
constexpr uint32_t kBiquadCode[] = {
    0x7C6B1B78, 0x2F080000, 0x40990024, 0x7C693850, 0x7D2A4B78, 0x7D074378,
    0x7C03542E, 0x34E7FFFF, 0xD00A0000, 0x394A0004, 0x4082FFF0, 0x7CAA0E70,
    0x7D4A0195, 0x40810080, 0x38E60008, 0x7C8B2050, 0x7D455378, 0xC187FFFC,
    0x2F080000, 0xC1670000, 0xC1470004, 0xC1270008, 0xC107000C, 0x40990048,
    0x7D2A4B78, 0x7D064378, 0xC00B0000, 0x34C6FFFF, 0xC0EA0000, 0xECC00272,
    0xEC003B3C, 0x7DA45C2E, 0xECED323A, 0xEC0D02FC, 0xEDA03ABA, 0xD1AA0000,
    0xC1AB0000, 0x394A0004, 0x7DA45D2E, 0xD00B0000, 0x4082FFC8, 0x34A5FFFF,
    0x38E70018, 0x396B0004, 0x4082FF94, 0x38600000, 0x4E800020};

float BeFloat(const uint8_t* p) {
  const uint32_t bits = xe::load_and_swap<uint32_t>(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void StoreBeFloat(uint8_t* p, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  xe::store_and_swap<uint32_t>(p, bits);
}

#pragma clang fp contract(off)
void BiquadCascade(uint8_t* state1, uint8_t* state2, int32_t sections,
                   const uint8_t* coefficients, uint8_t* samples, int32_t n) {
  for (int32_t section = 0; section < sections && n > 0; ++section) {
    const uint8_t* c = coefficients + 4 + 24 * section;
    const float a1 = BeFloat(c), a2 = BeFloat(c + 4), b0 = BeFloat(c + 8),
                b1 = BeFloat(c + 12), b2 = BeFloat(c + 16);
    float s1 = BeFloat(state1 + 4 * section);
    float s2 = BeFloat(state2 + 4 * section);
    for (int32_t i = 0; i < n; ++i) {
      const float x = BeFloat(samples + 4 * i);
      const float s1b1 = s1 * b1;
      float w = -std::fma(s1, a1, -x);
      const float tail = std::fma(s2, b2, s1b1);
      w = -std::fma(s2, a2, -w);
      StoreBeFloat(samples + 4 * i, std::fma(w, b0, tail));
      s2 = s1;
      s1 = w;
    }
    StoreBeFloat(state1 + 4 * section, s1);
    StoreBeFloat(state2 + 4 * section, s2);
  }
}

// Mode 1: the whole function.
void BiquadReplacement(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  uint8_t* membase = ctx->virtual_membase;
  const int32_t n = int32_t(ctx->r[8]);
  uint8_t* destination = membase + uint32_t(ctx->r[9]);
  if (n > 0) {
    const uint8_t* source = membase + uint32_t(ctx->r[7]);
    for (int32_t i = 0; i < n; ++i) {  // Forward, as the guest copies.
      std::memcpy(destination + 4 * i, source + 4 * i, 4);
    }
  }
  BiquadCascade(membase + uint32_t(ctx->r[3]), membase + uint32_t(ctx->r[4]),
                int32_t(ctx->r[5]) / 2, membase + uint32_t(ctx->r[6]),
                destination, n);
  ctx->r[3] = 0;
}

// Mode 2: the guest code runs; the entry hook computes the native result on
// copies and the exit hook compares it with what the guest code left.
struct BiquadCheck {
  uint32_t thread_id = 0;  // 0 = no call pending
  uint32_t state1 = 0, state2 = 0, destination = 0;
  int32_t sections = 0, n = 0;
  std::vector<uint8_t> expect_state1, expect_state2, expect_samples;
  uint64_t calls = 0, samples = 0, differing = 0, skipped = 0;
  uint32_t max_ulp = 0;
} g_biquad;

uint32_t UlpDistance(const uint8_t* a, const uint8_t* b) {
  auto ordered = [](uint32_t bits) {
    return int64_t(bits & 0x7FFFFFFFu) * ((bits >> 31) ? -1 : 1);
  };
  const int64_t d = ordered(xe::load_and_swap<uint32_t>(a)) -
                    ordered(xe::load_and_swap<uint32_t>(b));
  return uint32_t(std::min<int64_t>(d < 0 ? -d : d, UINT32_MAX));
}

void BiquadCheckEntry(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  BiquadCheck& check = g_biquad;
  const int32_t n = int32_t(ctx->r[8]);
  const int32_t sections = int32_t(ctx->r[5]) / 2;
  if (check.thread_id || n <= 0 || n > 8192 || sections <= 0 ||
      sections > 32) {
    ++check.skipped;
    return;
  }
  const uint8_t* membase = ctx->virtual_membase;
  check.state1 = uint32_t(ctx->r[3]);
  check.state2 = uint32_t(ctx->r[4]);
  check.destination = uint32_t(ctx->r[9]);
  check.sections = sections;
  check.n = n;
  const uint8_t* source = membase + uint32_t(ctx->r[7]);
  check.expect_samples.assign(source, source + size_t(n) * 4);
  check.expect_state1.assign(membase + check.state1,
                             membase + check.state1 + size_t(sections) * 4);
  check.expect_state2.assign(membase + check.state2,
                             membase + check.state2 + size_t(sections) * 4);
  BiquadCascade(check.expect_state1.data(), check.expect_state2.data(),
                sections, membase + uint32_t(ctx->r[6]),
                check.expect_samples.data(), n);
  check.thread_id = ctx->thread_id ? ctx->thread_id : 1;
}

void BiquadCheckExit(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  BiquadCheck& check = g_biquad;
  if (!check.thread_id ||
      check.thread_id != (ctx->thread_id ? ctx->thread_id : 1)) {
    return;
  }
  const uint8_t* membase = ctx->virtual_membase;
  auto compare = [&check](const uint8_t* guest,
                          const std::vector<uint8_t>& native) {
    for (size_t i = 0; i < native.size(); i += 4) {
      const uint32_t ulp = UlpDistance(guest + i, native.data() + i);
      check.differing += ulp != 0;
      check.max_ulp = std::max(check.max_ulp, ulp);
    }
  };
  compare(membase + check.destination, check.expect_samples);
  compare(membase + check.state1, check.expect_state1);
  compare(membase + check.state2, check.expect_state2);
  check.samples += uint64_t(check.n) * check.sections;
  check.thread_id = 0;
  if (++check.calls % 2000 == 0) {
    XELOGI(
        "XDROID-NATIVE biquad check: {} calls, {} sample steps, {} values differ "
        "from the guest code (max {} ulp), {} calls skipped",
        check.calls, check.samples, check.differing, check.max_ulp,
        check.skipped);
  }
}

// XAudio2 ramped mix, guest 0x823A73F8(destination, source, gain, step,
// floats): destination += source * gain over blocks of 16 floats, where gain
// is four lanes that advance by 4 * step per vector. VMX code: every vaddfp
// and vmaddfp128 of the translated loop carries the NaN and denormal fix-ups
// of the general case. Same operations here (fused multiply-add, the VMX
// FPCR of this guest thread), and like the guest it rounds the length up to
// 16 floats and reads a block before it writes it.
constexpr uint32_t kMixRamp = 0x823A73F8;
constexpr uint32_t kMixRampExit = 0x823A74EC;  // ld r31, -8(r1) (every path)
constexpr uint32_t kMixRampCode[] = {
    0xFBE1FFF8, 0x54EAD9FE, 0x39600000, 0x2B0A0000, 0x419A0018, 0x55693830,
    0x7C091A2C, 0x396B0001, 0x7F0B5040, 0x4198FFF0, 0x100030C3, 0x54EBF0BE,
    0x1160000A, 0x11A028C3, 0x118D000A, 0x2B0B0000, 0x114B000A, 0x100B580A,
    0x116D580A, 0x114D500A, 0x419A00A4, 0x396BFFFF, 0x7CA41850, 0x5569F0BE,
    0x39430030, 0x39640020, 0x39290001, 0x3BE00010, 0x3860FFE0, 0x3880FFF0,
    0x390AFFD0, 0x110D6C84, 0x38EAFFE0, 0x112050C3, 0x7CC55A14, 0x108BF8C3,
    0x106B18C3, 0x10EC6484, 0x10CB5C84, 0x11244AAE, 0x104040C3, 0x3929FFFF,
    0x1103122E, 0x10A058C3, 0x108B20C3, 0x11AD000A, 0x102038C3, 0x396B0040,
    0x106030C3, 0x10E409EE, 0x10C519AE, 0x2B090000, 0x118C000A, 0x116B000A,
    0x114A000A, 0x112051C3, 0x394A0040, 0x110041C3, 0x10E039C3, 0x10C031C3,
    0x409AFF88, 0xEBE1FFF8, 0x4E800020};

// The FPCR the translated VMX code of this guest thread runs with.
struct ScopedVmxFpcr {
  explicit ScopedVmxFpcr(xe::cpu::ppc::PPCContext* ctx)
      : old(__builtin_arm_rsr64("fpcr")) {
    const auto* backend_context =
        reinterpret_cast<const xe::cpu::backend::a64::A64BackendContext*>(
            reinterpret_cast<intptr_t>(ctx) -
            sizeof(xe::cpu::backend::a64::A64BackendContext));
    __builtin_arm_wsr64("fpcr", backend_context->fpcr_vmx);
  }
  ~ScopedVmxFpcr() { __builtin_arm_wsr64("fpcr", old); }
  uint64_t old;
};

float32x4_t LoadBe4(const uint8_t* p) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p)));
}

void StoreBe4(uint8_t* p, float32x4_t value) {
  vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(value)));
}

uint32_t MixRampBytes(uint32_t floats) {
  const uint32_t vectors = floats >> 2;
  return vectors ? (((vectors - 1) >> 2) + 1) * 64 : 0;
}

// Pointers already rounded down to 16 bytes, as lvx and stvx do.
void MixRamp(uint8_t* destination, const uint8_t* source, const uint8_t* gain,
             const uint8_t* step, uint32_t floats) {
  const float32x4_t step1 = LoadBe4(step);
  const float32x4_t step2 = vaddq_f32(step1, step1);
  float32x4_t gain0 = LoadBe4(gain);
  float32x4_t gain1 = vaddq_f32(gain0, step1);
  const float32x4_t step3 = vaddq_f32(step2, step1);
  const float32x4_t step4 = vaddq_f32(step2, step2);
  float32x4_t gain2 = vaddq_f32(gain0, step2);
  float32x4_t gain3 = vaddq_f32(gain0, step3);
  for (uint32_t left = MixRampBytes(floats); left;
       left -= 64, destination += 64, source += 64) {
    const float32x4_t d3 = vfmaq_f32(LoadBe4(destination + 48),
                                     LoadBe4(source + 48), gain3);
    const float32x4_t d0 =
        vfmaq_f32(LoadBe4(destination), LoadBe4(source), gain0);
    const float32x4_t d1 = vfmaq_f32(LoadBe4(destination + 16),
                                     LoadBe4(source + 16), gain1);
    const float32x4_t d2 = vfmaq_f32(LoadBe4(destination + 32),
                                     LoadBe4(source + 32), gain2);
    gain0 = vaddq_f32(gain0, step4);
    gain1 = vaddq_f32(gain1, step4);
    gain2 = vaddq_f32(gain2, step4);
    gain3 = vaddq_f32(gain3, step4);
    StoreBe4(destination + 48, d3);
    StoreBe4(destination, d0);
    StoreBe4(destination + 16, d1);
    StoreBe4(destination + 32, d2);
  }
}

void MixRampReplacement(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  uint8_t* membase = ctx->virtual_membase;
  ScopedVmxFpcr fpcr(ctx);
  MixRamp(membase + (uint32_t(ctx->r[3]) & ~15u),
          membase + (uint32_t(ctx->r[4]) & ~15u),
          membase + (uint32_t(ctx->r[5]) & ~15u),
          membase + (uint32_t(ctx->r[6]) & ~15u), uint32_t(ctx->r[7]));
}

struct MixRampCheck {
  uint32_t thread_id = 0;  // 0 = no call pending
  uint32_t destination = 0;
  std::vector<uint8_t> expect;
  uint64_t calls = 0, floats = 0, differing = 0, skipped = 0;
  uint32_t max_ulp = 0;
} g_mix_ramp;

void MixRampCheckEntry(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  MixRampCheck& check = g_mix_ramp;
  const uint32_t destination = uint32_t(ctx->r[3]) & ~15u;
  const uint32_t source = uint32_t(ctx->r[4]) & ~15u;
  const uint32_t bytes = MixRampBytes(uint32_t(ctx->r[7]));
  // A source that overlaps the destination would need the guest's exact
  // read and write order on shared memory: not checked on copies.
  if (check.thread_id || !bytes || bytes > 65536 ||
      (source < destination + bytes && destination < source + bytes)) {
    ++check.skipped;
    return;
  }
  const uint8_t* membase = ctx->virtual_membase;
  check.destination = destination;
  check.expect.assign(membase + destination, membase + destination + bytes);
  {
    ScopedVmxFpcr fpcr(ctx);
    MixRamp(check.expect.data(), membase + source,
            membase + (uint32_t(ctx->r[5]) & ~15u),
            membase + (uint32_t(ctx->r[6]) & ~15u), uint32_t(ctx->r[7]));
  }
  check.thread_id = ctx->thread_id ? ctx->thread_id : 1;
}

void MixRampCheckExit(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  MixRampCheck& check = g_mix_ramp;
  if (!check.thread_id ||
      check.thread_id != (ctx->thread_id ? ctx->thread_id : 1)) {
    return;
  }
  const uint8_t* guest = ctx->virtual_membase + check.destination;
  for (size_t i = 0; i < check.expect.size(); i += 4) {
    const uint32_t ulp = UlpDistance(guest + i, check.expect.data() + i);
    check.differing += ulp != 0;
    check.max_ulp = std::max(check.max_ulp, ulp);
  }
  check.floats += check.expect.size() / 4;
  check.thread_id = 0;
  if (++check.calls % 20000 == 0) {
    XELOGI(
        "XDROID-NATIVE mix check: {} calls, {} floats, {} values differ from "
        "the guest code (max {} ulp), {} calls skipped",
        check.calls, check.floats, check.differing, check.max_ulp,
        check.skipped);
  }
}

// XAudio2 linear resampler for 16-bit mono sources, guest 0x823BA0A0(source,
// -, destination, samples, state): 8 output samples per step from a 32.32
// phase, each (next - current) * (fraction * volume) + current * volume with
// a volume that ramps from state+0x24 to state+0x28 over the call. Writes
// the frames consumed (+8), the floats written (+0x1C), the volume reached
// (+0x24) and the fractional position (+0x30). Same scalar and vector steps
// as the guest code, in its order.
constexpr uint32_t kResample = 0x823BA0A0;
constexpr uint32_t kResampleExit = 0x823BA318;  // b epilogue (after stores)
constexpr uint32_t kResampleCode[] = {
    0x7D8802A6, 0x4BFC8971, 0x9421FF70, 0x7CFF3B78, 0xC19F0024, 0xD1810050,
    0x7C001A2C, 0x3D608204, 0xC17F0030, 0xC15F002C, 0x7CCA07B4, 0xC01F0028,
    0x39210050, 0xD01F0024, 0x39010050, 0x38E10058, 0x1180038C, 0xC9AB4118,
    0x3D608205, 0xFD6B0372, 0xF9410058, 0xFDAA0372, 0x396BE160, 0x3BC10050,
    0x10C1038C, 0x3BA10058, 0x39400004, 0x3B810058, 0x116058C3, 0x3D608205,
    0x3B610050, 0x396BE150, 0x38800000, 0xFD605E5E, 0xFD406E5E, 0xEDA06028,
    0xC8010058, 0xFC00069C, 0x110058C3, 0x3D608200, 0xFC000018, 0xEC0D0024,
    0xC1AB1838, 0xD0010058, 0x7C004C0E, 0x1000028C, 0xEC000372, 0xD0010050,
    0x7DA0440E, 0x7D403C0E, 0x10A06A8C, 0xD97E0000, 0x1140528C, 0xD95D0000,
    0xE9610050, 0x7DAAE40E, 0xE9010058, 0x10E06A8C, 0x11A00484, 0x7C0ADC0E,
    0x38E00002, 0x1120028C, 0x3BC00010, 0x10073C84, 0x116A6AEE, 0x114A6A2E,
    0x11A00080, 0x110C012C, 0x10E06880, 0x1108692C, 0x100D6880, 0x11AC6484,
    0x1108392C, 0x10800080, 0x10E80080, 0x11084880, 0x10074880, 0x39400008,
    0x79690022, 0x394AFFFF, 0x5529083C, 0x7D685A14, 0x7D291A14, 0x2B0A0000,
    0x7D293C0E, 0x118C48AC, 0x7D204C0E, 0x11AD48AC, 0x409AFFD8, 0x11206A4E,
    0x10683284, 0x1040624E, 0x10203284, 0x10E06ACE, 0x796A0022, 0x13E062CE,
    0x796B0020, 0x112F4B4A, 0x554A083C, 0x104F134A, 0x10EF3B4A, 0x7C6A1A14,
    0x107F1B0A, 0x103F0B0A, 0x7C8A1A78, 0x13EFFB4A, 0x554A0030, 0x2B0A0000,
    0x17C95090, 0x1122484A, 0x14475890, 0x14635090, 0x14215890, 0x10FF384A,
    0x1129F0EE, 0x10E7106E, 0x112029C3, 0x10E5F1C3, 0x419A000C, 0x39400080,
    0x7C0A1A2C, 0x38C6FFF8, 0x116B280A, 0x7C641B78, 0x114A280A, 0x38A50020,
    0x10002080, 0x11082080, 0x2F060000, 0x4199FF30, 0x895F000D, 0x813F0000,
    0x5548083E, 0x815F0004, 0x7D291850, 0x0CC80000, 0x7D294396, 0x7F095040,
    0x7D284B78, 0x41980008, 0x7D485378, 0x813F0014, 0x815F0018, 0x7D292850,
    0x911F0008, 0x5529F0BE, 0x7F095040, 0x40980008, 0x7D2A4B78, 0x7D635B78,
    0x915F001C, 0x4BFCB385, 0x3D608205, 0xC80BE140, 0xFC010032, 0xFC000018,
    0xD01F0030, 0x38210090, 0x4BFC874C};

double BeDouble(const uint8_t* p) {
  const uint64_t bits = xe::load_and_swap<uint64_t>(p);
  double value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

int64_t Fctidz(double value) {  // PowerPC saturation, not the host's.
  if (value != value) {
    return INT64_MIN;
  }
  if (value >= 9223372036854775808.0) {
    return INT64_MAX;
  }
  if (value <= -9223372036854775808.0) {
    return INT64_MIN;
  }
  return int64_t(value);
}

struct ResampleState {
  uint32_t consumed, written, volume_bits, position_bits;
};

uint32_t ResampleSteps(int32_t samples) {
  return samples > 8 ? uint32_t(samples + 7) / 8 : 1;
}

// |out| takes 32 bytes per step (the destination rounded down to 16 bytes).
ResampleState Resample(xe::cpu::ppc::PPCContext* ctx, uint8_t* out) {
  const uint8_t* membase = ctx->virtual_membase;
  const auto* backend_context =
      reinterpret_cast<const xe::cpu::backend::a64::A64BackendContext*>(
          reinterpret_cast<intptr_t>(ctx) -
          sizeof(xe::cpu::backend::a64::A64BackendContext));
  const uint64_t host_fpcr = __builtin_arm_rsr64("fpcr");
  __builtin_arm_wsr64("fpcr", backend_context->fpcr_fpu);

  uint32_t source = uint32_t(ctx->r[3]);
  uint32_t destination = uint32_t(ctx->r[5]);
  int32_t samples = int32_t(ctx->r[6]);
  const uint8_t* state = membase + uint32_t(ctx->r[7]);
  const float volume = BeFloat(state + 0x24);
  const float target = BeFloat(state + 0x28);
  const double to_fixed = BeDouble(membase + 0x82044118);
  uint64_t phase = uint64_t(Fctidz(double(BeFloat(state + 0x30)) * to_fixed));
  const uint64_t step = uint64_t(Fctidz(double(BeFloat(state + 0x2C)) * to_fixed));
  const float difference = float(double(target) - double(volume));
  const float volume_step =
      float(double(difference) / double(float(double(int64_t(samples)))));
  const float volume_step_8 =
      float(double(volume_step) * double(BeFloat(membase + 0x82001838)));

  __builtin_arm_wsr64("fpcr", backend_context->fpcr_vmx);
  const float32x4_t step_vector = vdupq_n_f32(volume_step);
  const float32x4_t volume_vector = vdupq_n_f32(volume);
  float32x4_t volume_high = vfmaq_f32(
      volume_vector, step_vector, LoadBe4(membase + 0x8204E160));
  float32x4_t volume_low = vfmaq_f32(
      volume_vector, step_vector, LoadBe4(membase + 0x8204E150));
  const float32x4_t volume_advance = vdupq_n_f32(volume_step_8);
  const uint32_t step_low = uint32_t(step), phase_low = uint32_t(phase);
  const uint32_t ramp[4] = {0, step_low, step_low * 2, step_low * 3};
  uint32x4_t fraction_low =
      vaddq_u32(vld1q_u32(ramp), vdupq_n_u32(phase_low));
  uint32x4_t fraction_high = vaddq_u32(
      vaddq_u32(vld1q_u32(ramp), vdupq_n_u32(step_low * 4)),
      vdupq_n_u32(phase_low));
  const uint32x4_t fraction_advance = vdupq_n_u32(step_low * 8);
  do {
    int16_t current[8], next[8];
    for (int i = 0; i < 8; ++i) {
      const uint8_t* sample = membase + uint32_t(source + (uint32_t(phase >> 32) << 1));
      current[i] = int16_t(xe::load_and_swap<uint16_t>(sample));
      next[i] = int16_t(xe::load_and_swap<uint16_t>(sample + 2));
      phase += step;
    }
    const float32x4_t current_low =
        vcvtq_n_f32_s32(vmovl_s16(vld1_s16(current)), 15);
    const float32x4_t current_high =
        vcvtq_n_f32_s32(vmovl_s16(vld1_s16(current + 4)), 15);
    const float32x4_t next_low = vcvtq_n_f32_s32(vmovl_s16(vld1_s16(next)), 15);
    const float32x4_t next_high =
        vcvtq_n_f32_s32(vmovl_s16(vld1_s16(next + 4)), 15);
    const float32x4_t weight_low = vmulq_f32(
        vcvtq_n_f32_u32(vshrq_n_u32(fraction_low, 1), 31), volume_low);
    const float32x4_t weight_high = vmulq_f32(
        vcvtq_n_f32_u32(vshrq_n_u32(fraction_high, 1), 31), volume_high);
    StoreBe4(out, vfmaq_f32(vmulq_f32(current_low, volume_low),
                            vsubq_f32(next_low, current_low), weight_low));
    StoreBe4(out + 16,
             vfmaq_f32(vmulq_f32(current_high, volume_high),
                       vsubq_f32(next_high, current_high), weight_high));
    source += uint32_t(phase >> 32) << 1;
    phase &= 0xFFFFFFFFu;
    samples -= 8;
    volume_high = vaddq_f32(volume_high, volume_advance);
    volume_low = vaddq_f32(volume_low, volume_advance);
    destination += 32;
    out += 32;
    fraction_high = vaddq_u32(fraction_high, fraction_advance);
    fraction_low = vaddq_u32(fraction_low, fraction_advance);
  } while (samples > 0);

  __builtin_arm_wsr64("fpcr", backend_context->fpcr_fpu);
  ResampleState result;
  const uint32_t frame_bytes = uint32_t(state[0xD]) << 1;
  const uint32_t consumed =
      frame_bytes ? (source - xe::load_and_swap<uint32_t>(state)) / frame_bytes
                  : 0;
  result.consumed =
      std::min(consumed, xe::load_and_swap<uint32_t>(state + 4));
  result.written =
      std::min((destination - xe::load_and_swap<uint32_t>(state + 0x14)) >> 2,
               xe::load_and_swap<uint32_t>(state + 0x18));
  std::memcpy(&result.volume_bits, &target, 4);
  const float position =
      float(double(int64_t(phase)) * BeDouble(membase + 0x8204E140));
  std::memcpy(&result.position_bits, &position, 4);
  __builtin_arm_wsr64("fpcr", host_fpcr);
  return result;
}

void ResampleReplacement(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  uint8_t* membase = ctx->virtual_membase;
  uint8_t* state = membase + uint32_t(ctx->r[7]);
  const ResampleState result =
      Resample(ctx, membase + (uint32_t(ctx->r[5]) & ~15u));
  xe::store_and_swap<uint32_t>(state + 8, result.consumed);
  xe::store_and_swap<uint32_t>(state + 0x1C, result.written);
  xe::store_and_swap<uint32_t>(state + 0x24, result.volume_bits);
  xe::store_and_swap<uint32_t>(state + 0x30, result.position_bits);
}

struct ResampleCheck {
  uint32_t thread_id = 0;  // 0 = no call pending
  uint32_t destination = 0, state = 0;
  ResampleState expect_state = {};
  std::vector<uint8_t> expect;
  // The inputs as this check read them (source, limits, target volume, step).
  uint8_t inputs[0x34] = {};
  uint64_t calls = 0, floats = 0, differing = 0, state_differing = 0,
           skipped = 0, raced = 0;
  uint32_t max_ulp = 0;
} g_resample;

void ResampleCheckEntry(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  ResampleCheck& check = g_resample;
  const uint32_t steps = ResampleSteps(int32_t(ctx->r[6]));
  if (check.thread_id || steps > 2048) {
    ++check.skipped;
    return;
  }
  check.destination = uint32_t(ctx->r[5]) & ~15u;
  check.state = uint32_t(ctx->r[7]);
  check.expect.resize(size_t(steps) * 32);
  std::memcpy(check.inputs, ctx->virtual_membase + check.state,
              sizeof(check.inputs));
  check.expect_state = Resample(ctx, check.expect.data());
  check.thread_id = ctx->thread_id ? ctx->thread_id : 1;
}

void ResampleCheckExit(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  ResampleCheck& check = g_resample;
  if (!check.thread_id ||
      check.thread_id != (ctx->thread_id ? ctx->thread_id : 1)) {
    return;
  }
  const uint8_t* membase = ctx->virtual_membase;
  const uint8_t* guest = membase + check.destination;
  const uint8_t* state = membase + check.state;
  // Another guest thread changed the voice (source, limits, target volume or
  // step) while the mixer was in the function: the guest code may have read
  // other inputs than this check did, so the call proves nothing.
  if (std::memcmp(check.inputs, state, 8) ||
      std::memcmp(check.inputs + 0xC, state + 0xC, 0x10) ||
      std::memcmp(check.inputs + 0x28, state + 0x28, 8)) {
    ++check.raced;
    check.thread_id = 0;
    return;
  }
  for (size_t i = 0; i < check.expect.size(); i += 4) {
    const uint32_t ulp = UlpDistance(guest + i, check.expect.data() + i);
    check.differing += ulp != 0;
    check.max_ulp = std::max(check.max_ulp, ulp);
  }
  check.state_differing +=
      (xe::load_and_swap<uint32_t>(state + 8) != check.expect_state.consumed) +
      (xe::load_and_swap<uint32_t>(state + 0x1C) !=
       check.expect_state.written) +
      (xe::load_and_swap<uint32_t>(state + 0x24) !=
       check.expect_state.volume_bits) +
      (xe::load_and_swap<uint32_t>(state + 0x30) !=
       check.expect_state.position_bits);
  check.floats += check.expect.size() / 4;
  check.thread_id = 0;
  if (++check.calls % 5000 == 0) {
    XELOGI(
        "XDROID-NATIVE resample check: {} calls, {} floats, {} values differ "
        "from the guest code (max {} ulp), {} state words differ, {} calls "
        "skipped, {} with inputs changed under them",
        check.calls, check.floats, check.differing, check.max_ulp,
        check.state_differing, check.skipped, check.raced);
  }
}

// Installs |replacement| over the guest function at |address| (mode 1) or the
// check pair around it (mode 2), when its code is the one this was written
// against.
void InstallNative(xe::Emulator* emulator, const char* name, uint32_t address,
                   const uint32_t* code, size_t code_words,
                   uint32_t exit_address,
                   xe::cpu::BuiltinFunction::Handler replacement,
                   xe::cpu::BuiltinFunction::Handler check_entry,
                   xe::cpu::BuiltinFunction::Handler check_exit) {
  const int32_t mode = cvars::ac6_native_audio;
  if (mode != 1 && mode != 2) {
    return;
  }
  const uint8_t* membase = emulator->memory()->virtual_membase();
  for (size_t i = 0; i < code_words; ++i) {
    if (Load32(membase, address + uint32_t(i) * 4) != code[i]) {
      XELOGW("XDroid360: AC6 native {} not installed (code differs)", name);
      return;
    }
  }
  xe::cpu::Processor* processor = emulator->processor();
  if (mode == 1) {
    processor->SetGuestHook(
        address, processor->DefineBuiltin(name, replacement, nullptr, nullptr),
        xe::cpu::Processor::GuestHookKind::kReplace);
  } else {
    processor->SetGuestHook(
        address, processor->DefineBuiltin(name, check_entry, nullptr, nullptr),
        xe::cpu::Processor::GuestHookKind::kInstruction);
    processor->SetGuestHook(
        exit_address,
        processor->DefineBuiltin(name, check_exit, nullptr, nullptr),
        xe::cpu::Processor::GuestHookKind::kInstruction);
  }
  XELOGI("XDroid360: AC6 native {} mode {} installed", name, mode);
}

void OnLaunch(xe::Emulator* emulator, uint32_t title_id) {
  if (title_id != kAc6TitleId) {
    return;
  }
  // The job pool hooks (HUD-1 arms, storage race) share the Submit entry.
  const bool jobs = InstallAc6Jobs(emulator);
  if (jobs) {
    InstallAc6JobTracer();
  }
  // Only for the executable this was worked out on: both effect manager
  // vtables must route vtbl[+0x04] to the dispatcher and vtbl[+0x100] to the
  // known draws.
  const uint8_t* membase = emulator->memory()->virtual_membase();
  const bool layout_ok = Load32(membase, kVtblX360 + 4) == kEffDispatcher &&
                         Load32(membase, kVtblAce6 + 4) == kEffDispatcher &&
                         Load32(membase, kVtblX360 + 0x100) == kEffDrawX360 &&
                         Load32(membase, kVtblAce6 + 0x100) == kEffDrawAce6;
  // Same executable as AC6_recomp: its fixed timing addresses apply.
  if (layout_ok) {
    InstallAc6Timing(emulator);
  }
  InstallNative(emulator, "XDroidAc6Biquad", kBiquad, kBiquadCode,
                std::size(kBiquadCode), kBiquadExit, BiquadReplacement,
                BiquadCheckEntry, BiquadCheckExit);
  InstallNative(emulator, "XDroidAc6MixRamp", kMixRamp, kMixRampCode,
                std::size(kMixRampCode), kMixRampExit, MixRampReplacement,
                MixRampCheckEntry, MixRampCheckExit);
  InstallNative(emulator, "XDroidAc6Resample", kResample, kResampleCode,
                std::size(kResampleCode), kResampleExit, ResampleReplacement,
                ResampleCheckEntry, ResampleCheckExit);
  // HUD set pick (HUD-1): hooked whenever both instruction words match, so
  // the repeat counter runs in every arm; the wait itself is live.
  if (Load32(membase, kHudSetPickLoad) == kHudSetPickLoadWord &&
      Load32(membase, kHudCounterStore) == kHudCounterStoreWord) {
    xe::cpu::Processor* hud_processor = emulator->processor();
    hud_processor->SetGuestHook(
        kHudCounterStore,
        hud_processor->DefineBuiltin("XDroidAc6HudCounter",
                                     HudCounterStoreHook, nullptr, nullptr),
        xe::cpu::Processor::GuestHookKind::kInstruction);
    hud_processor->SetGuestHook(
        kHudSetPickLoad,
        hud_processor->DefineBuiltin("XDroidAc6HudSetPick", HudSetPickHook,
                                     nullptr, nullptr),
        xe::cpu::Processor::GuestHookKind::kInstruction);
    XELOGI("XDroid360: AC6 HUD set pick hooks installed");
  }
  if (Load32(membase, kResResolveMiss) == kResResolveMissWord &&
      Load32(membase, kResResolveHit) == kResResolveHitWord) {
    xe::cpu::Processor* res_processor = emulator->processor();
    res_processor->SetGuestHook(
        kResResolveMiss,
        res_processor->DefineBuiltin("XDroidAc6ResMiss", ResMissHook, nullptr,
                                     nullptr),
        xe::cpu::Processor::GuestHookKind::kInstruction);
    res_processor->SetGuestHook(
        kResResolveHit,
        res_processor->DefineBuiltin("XDroidAc6ResHit", ResHitHook, nullptr,
                                     nullptr),
        xe::cpu::Processor::GuestHookKind::kInstruction);
  } else {
    XELOGW("XDroid360: AC6 HUD set pick hooks not installed (code differs)");
  }
  bool effect = cvars::ac6_effect_mode_fix;
  if (effect && !layout_ok) {
    XELOGW(
        "XDroid360: AC6 effect mode fix not applied - executable layout does "
        "not match");
    effect = false;
    cvars::ac6_effect_mode_fix = false;
  }
  if (!jobs && !effect) {
    return;
  }
  xe::cpu::Processor* processor = emulator->processor();
  processor->SetGuestHook(
      kPoolSubmit,
      processor->DefineBuiltin("XDroidAc6SubmitHook", SubmitEntryHook, nullptr,
                               nullptr),
      xe::cpu::Processor::GuestHookKind::kEntry);
  if (!effect) {
    return;
  }
  processor->SetGuestHook(
      kEffDispatcher,
      processor->DefineBuiltin("XDroidAc6EffectDispatch",
                               DispatcherReplacement, nullptr, nullptr),
      xe::cpu::Processor::GuestHookKind::kReplace);
  XELOGI("XDroid360: AC6 effect mode fix installed");
}

}  // namespace

void InstallAc6Fixes(xe::Emulator* emulator) {
  // Shader storage starts translating the title's shaders before on_launch,
  // so the list is set here (also cleared for other titles).
  emulator->on_shader_storage_initialization.AddListener(
      [emulator](bool starting) {
        if (!starting) {
          return;
        }
        bool ac6 = emulator->title_id() == kAc6TitleId &&
                   cvars::ac6_cloud_deswizzle_fix;
        xe::gpu::SpirvShaderTranslator::SetTitleDeswizzleIdentityPsHashes(
            ac6 ? kAc6DeswizzleHashes : "");
        if (ac6) {
          XELOGI("XDroid360: AC6 cloud/effect de-swizzle fix enabled");
        }
      });
  emulator->on_launch.AddListener(
      [emulator](uint32_t title_id, const std::string_view) {
        OnLaunch(emulator, title_id);
      });
}

}  // namespace xdroid
