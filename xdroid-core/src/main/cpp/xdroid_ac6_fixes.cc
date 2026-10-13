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

#include <cstring>
#include <mutex>

#include "xdroid_ac6_jobs.h"

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/spirv_shader_translator.h"
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
  bool effect = cvars::ac6_effect_mode_fix;
  if (effect && (Load32(membase, kVtblX360 + 4) != kEffDispatcher ||
                 Load32(membase, kVtblAce6 + 4) != kEffDispatcher ||
                 Load32(membase, kVtblX360 + 0x100) != kEffDrawX360 ||
                 Load32(membase, kVtblAce6 + 0x100) != kEffDrawAce6)) {
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
