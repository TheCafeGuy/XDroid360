/**
 * XDroid360: Ace Combat 6 (4E4D07D1) job pool B ordering.
 *
 * Pool B (Submit 0x821D4A78, four worker threads entering 0x821D4F20) runs the
 * effect update/draw jobs, the async storage operations and other per-frame
 * work. On the console fixed core assignment kept a job's start close to its
 * submit; under emulation a slow or throttled host stretches that gap, and
 * the game has at least two known races across it (the effect mode race,
 * xdroid_ac6_fixes.cc, and race 2 below). HUD-1 (one-frame HUD garbage that
 * rises with thermal throttling) is suspected to be another one.
 *
 * This file tracks every pool B job (submit, start, end) through guest hooks:
 * the Submit entry, every `bl Submit` call site (found by scanning the code
 * section at launch), and the worker's indirect call of job->vtbl[1]
 * (bctrl at 0x821D4F7C) and its return. On top of that it offers live arms:
 *
 * - ac6_storage_submit_order_fix (race 2, on by default): twelve storage call
 *   sites store the job's operation code to [job+4] only after Submit, so a
 *   fast worker can read the previous operation. The code is stored before
 *   the call, as the retail title update does. Port of AC6_recomp 29e3dab1
 *   (src/ac6_backend_fixes/ac6_storage_submit_order_fix.cpp).
 * - ac6_job_start_order (arm A, off): Submit returns only once a worker has
 *   started the job (bounded), for jobs that read state the submitter changes
 *   right after. Classes whose callers store into the job after Submit (the
 *   race 2 direction, which waiting would make certain) are excluded, as are
 *   classes whose waits keep timing out.
 * - xdroid_cp_wait_jobs (arm C+, off): the command processor holds back the
 *   commands of a kick until the pool B jobs submitted before that kick have
 *   ended (bounded per kick and per frame), so the GPU never reads data a job
 *   has not written yet. Long-running classes are excluded.
 * - xdroid_vdswap_wait_cp (arm C-, off): VdSwap waits until the CP is at
 *   most xdroid_vdswap_wait_cp_max_lag swaps behind (bounded), so the guest
 *   cannot start refilling per-frame buffers the CP has not read yet.
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

#include "xdroid_ac6_jobs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "xdroid_time_coupling.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/emulator.h"
#include "xenia/gpu/xdroid_gpu_hooks.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/memory.h"

DEFINE_bool(ac6_storage_submit_order_fix, true,
            "Ace Combat 6: store each async storage job's operation code "
            "before the pool Submit instead of after it, so a fast worker "
            "never reads the previous operation (port of the AC6_recomp fix).",
            "XDroid360");
DEFINE_bool(ac6_job_start_order, false,
            "Ace Combat 6 (HUD-1 arm A): return from the job pool Submit only "
            "once a worker has started the job (bounded by "
            "ac6_job_start_wait_us). Live.",
            "XDroid360");
DEFINE_int32(ac6_job_start_wait_us, 2000,
             "Ace Combat 6: longest wait of ac6_job_start_order per Submit.",
             "XDroid360");
DEFINE_bool(xdroid_cp_wait_jobs, false,
            "Ace Combat 6 (HUD-1 arm C+): the command processor waits for the "
            "pool jobs submitted before each kick to end before executing it "
            "(bounded per kick and by xdroid_cp_wait_jobs_frame_us). Live.",
            "XDroid360");
DEFINE_int32(xdroid_cp_wait_jobs_frame_us, 2000,
             "Most time xdroid_cp_wait_jobs may wait in one frame.",
             "XDroid360");
DEFINE_bool(xdroid_vdswap_wait_cp, false,
            "Ace Combat 6 (HUD-1 arm C-): VdSwap waits until the command "
            "processor is at most xdroid_vdswap_wait_cp_max_lag swaps behind "
            "(bounded by xdroid_vdswap_wait_cp_us). Live.",
            "XDroid360");
DEFINE_int32(xdroid_vdswap_wait_cp_max_lag, 0,
             "Swaps the command processor may still owe when VdSwap returns "
             "under xdroid_vdswap_wait_cp (0 = it processed the previous "
             "swap). Live.",
             "XDroid360");
DEFINE_int32(xdroid_vdswap_wait_cp_us, 4000,
             "Longest wait of xdroid_vdswap_wait_cp per VdSwap. Live.", "XDroid360");

DEFINE_bool(xdroid_cp_defer_to_swap, false,
            "HUD-1 arm H1 (temp): the command processor does not execute a "
            "kick of a frame until the guest has called VdSwap for that frame "
            "(bounded by xdroid_cp_defer_to_swap_us; after a timeout it stops "
            "waiting for the rest of the frame). Live.",
            "XDroid360");
DEFINE_int32(xdroid_cp_defer_to_swap_us, 20000,
             "Longest wait of xdroid_cp_defer_to_swap per frame. Live.",
             "XDroid360");

namespace xdroid {
namespace {

constexpr uint32_t kPoolSubmit = 0x821D4A78;
constexpr uint32_t kWorkerCall = 0x821D4F7C;    // bctrl job->vtbl[1]
constexpr uint32_t kWorkerReturn = 0x821D4F80;
constexpr uint32_t kBctrl = 0x4E800421;

// AC6_recomp ac6_storage_submit_order_fix.cpp: bl Submit sites and the
// operation code each one stores after the call (0 = carried in r29).
struct StorageSite {
  uint32_t bl;
  uint32_t op;
};
constexpr StorageSite kStorageSites[] = {
    {0x821D3C8C, 0x2}, {0x821D3D0C, 0x3}, {0x821D3DC8, 0x6},
    {0x821D3E58, 0x7}, {0x821D3F70, 0xA}, {0x821D4030, 0xB},
    {0x821D40F0, 0xC}, {0x821C7538, 0xD}, {0x821C8EC4, 0x8},
    {0x821C8F78, 0x9}, {0x821C9298, 0x10}, {0x821C5A78, 0x0},
};
constexpr size_t kStorageSiteCount =
    sizeof(kStorageSites) / sizeof(kStorageSites[0]);

// One hooked guest address; an address can be both the instruction after a
// Submit call and a storage bl.
struct Site {
  uint32_t address = 0;
  int storage = -1;           // pre-call: index into kStorageSites
  uint32_t after_bl = 0;      // post-call: the bl Submit before this address
  bool after_storage = false;
};

const uint8_t* g_membase = nullptr;
uint32_t g_code_low = 0;
uint32_t g_code_high = 0;
std::vector<std::unique_ptr<Site>> g_sites;

Ac6JobRecord g_ring[kAc6JobRing];
std::atomic<uint32_t> g_seq{0};
Ac6JobClass g_classes[kAc6MaxClasses];

// Per guest thread (by id): the last job it submitted, and as a worker the
// job it is running.
constexpr uint32_t kThreadSlots = 1024;
std::atomic<uint32_t> g_thread_submit[kThreadSlots];
std::atomic<uint32_t> g_thread_running[kThreadSlots];

std::atomic<bool> g_storage_logged[kStorageSiteCount];
std::atomic<uint64_t> g_storage_engaged{0};
std::atomic<uint32_t> g_start_mismatch{0};
// Starts that found the same job object submitted more than once, unstarted.
std::atomic<uint64_t> g_resubmits{0};

// Pool bookkeeping: words of a job that change between Submit returning and
// the start in (nearly) every job, whatever the class (queue links, flags the
// worker sets on dequeue). They are not caller stores.
std::atomic<uint32_t> g_word_changes[64];
std::atomic<uint32_t> g_diffs{0};

// Kicks (ring buffer write pointer moves) and the submit sequence at each.
struct Kick {
  std::atomic<uint32_t> write_ptr{0};
  std::atomic<uint32_t> seq{0};
};
constexpr uint32_t kKickRing = 64;
Kick g_kicks[kKickRing];
std::atomic<uint32_t> g_kick_count{0};

// CP thread only.
uint32_t g_cp_frame = UINT32_MAX;
uint64_t g_cp_frame_waited_us = 0;

std::atomic<uint64_t> g_guest_swaps{0};
std::atomic<uint64_t> g_swap_lag[4];
// xdroid_vdswap_wait_cp: waits, timeouts, waited microseconds.
std::atomic<uint64_t> g_swap_wait[3];
// xdroid_cp_defer_to_swap: waits that saw the swap, timeouts, waited us.
std::atomic<uint64_t> g_defer_wait[3];
uint32_t g_defer_frame = UINT32_MAX;
bool g_defer_gave_up = false;

std::atomic<void (*)(const Ac6JobRecord&, uint64_t)> g_on_start{nullptr};
std::atomic<void (*)(uint32_t)> g_on_glitch{nullptr};

uint32_t Load32(uint32_t address) {
  return xe::load_and_swap<uint32_t>(g_membase + address);
}

bool IsGuestHeap(uint32_t address) {
  return address >= 0x40000000u && address < 0xC0000000u;
}

bool IsCode(uint32_t address) {
  return address >= g_code_low && address < g_code_high && !(address & 3);
}

uint32_t ThreadSlot(const xe::cpu::ppc::PPCContext* ctx) {
  return ctx->thread_id & (kThreadSlots - 1);
}

uint32_t ClassFor(uint32_t vt, uint32_t fn) {
  for (uint32_t i = 0; i < kAc6MaxClasses; ++i) {
    uint32_t current = g_classes[i].vt.load(std::memory_order_acquire);
    if (current == vt) {
      return i;
    }
    if (!current) {
      uint32_t expected = 0;
      if (g_classes[i].vt.compare_exchange_strong(expected, vt,
                                                  std::memory_order_acq_rel)) {
        g_classes[i].fn.store(fn, std::memory_order_relaxed);
        return i;
      }
      if (expected == vt) {
        return i;
      }
    }
  }
  return kAc6MaxClasses - 1;  // Overflow class, shared.
}

uint64_t BookkeepingMask() {
  const uint32_t diffs = g_diffs.load(std::memory_order_relaxed);
  if (diffs < 100) {
    return ~uint64_t(0);  // Still learning: count nothing as a caller store.
  }
  uint64_t mask = 0;
  for (uint32_t i = 0; i < 64; ++i) {
    if (uint64_t(g_word_changes[i].load(std::memory_order_relaxed)) * 10 >=
        uint64_t(diffs) * 9) {
      mask |= uint64_t(1) << i;
    }
  }
  return mask;
}

Ac6JobRecord* RecordFor(uint32_t seq) {
  Ac6JobRecord& record = g_ring[seq % kAc6JobRing];
  return record.seq.load(std::memory_order_acquire) == seq ? &record : nullptr;
}

// Pre-call hook at a storage bl: store the operation code before Submit.
void StorageBeforeSubmit(const Site& site, xe::cpu::ppc::PPCContext* ctx) {
  const uint32_t job = uint32_t(ctx->r[5]);
  const StorageSite& storage = kStorageSites[site.storage];
  const uint32_t op = storage.op ? storage.op : uint32_t(ctx->r[29]);
  if (!IsGuestHeap(job)) {
    return;
  }
  const uint32_t vt = Load32(job);
  if (IsCode(vt) || (vt >= 0x82000000u && vt < 0x82A00000u)) {
    g_classes[ClassFor(vt, Load32(vt + 4))].storage.store(
        true, std::memory_order_relaxed);
  }
  if (!cvars::ac6_storage_submit_order_fix) {
    return;
  }
  xe::store_and_swap<uint32_t>(const_cast<uint8_t*>(g_membase) + job + 4, op);
  const uint64_t total =
      g_storage_engaged.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!g_storage_logged[site.storage].exchange(true,
                                               std::memory_order_relaxed)) {
    XELOGI(
        "XDroid360: AC6 storage submit order fix engaged at {:08X} (op {:X}), "
        "{} so far",
        storage.bl, op, total);
  }
}

// Post-call hook after a bl Submit, on the submitting thread.
void AfterSubmit(const Site& site, xe::cpu::ppc::PPCContext* ctx) {
  const uint32_t seq = g_thread_submit[ThreadSlot(ctx)].exchange(
      0, std::memory_order_relaxed);
  Ac6JobRecord* record = seq ? RecordFor(seq) : nullptr;
  if (!record) {
    return;
  }
  // Snapshot the job as the caller leaves Submit; the start compares against
  // it to see what the caller stores afterwards.
  uint32_t state = 0;
  if (record->snap_state.compare_exchange_strong(state, 1,
                                                 std::memory_order_acq_rel)) {
    std::memcpy(record->snapshot, g_membase + record->job,
                record->snapshot_len);
    state = 1;
    record->snap_state.compare_exchange_strong(state, 2,
                                               std::memory_order_acq_rel);
  }
  if (!cvars::ac6_job_start_order || site.after_storage) {
    return;
  }
  Ac6JobClass& job_class = g_classes[record->cls];
  if (job_class.storage.load(std::memory_order_relaxed) ||
      job_class.no_start_wait.load(std::memory_order_relaxed)) {
    return;
  }
  // Callers that store into the job after Submit need the start to come
  // after those stores: waiting would make the race certain.
  const uint64_t jobs = job_class.jobs.load(std::memory_order_relaxed);
  const uint64_t post_stores =
      job_class.post_store_jobs.load(std::memory_order_relaxed);
  if (post_stores >= 3 && post_stores * 100 >= jobs) {
    if (!job_class.no_start_wait.exchange(true, std::memory_order_relaxed)) {
      XELOGI(
          "XDroid360: AC6 job start order skips class vt {:08X} (callers store "
          "after Submit in {} of {} jobs)",
          record->vt, post_stores, jobs);
    }
    return;
  }
  if (record->start_us.load(std::memory_order_acquire)) {
    return;
  }
  const uint64_t waits =
      job_class.start_waits.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t deadline =
      Ac6NowUs() + uint64_t(std::max(cvars::ac6_job_start_wait_us, 0));
  while (!record->start_us.load(std::memory_order_acquire) &&
         record->seq.load(std::memory_order_acquire) == seq) {
    if (Ac6NowUs() >= deadline) {
      const uint64_t timeouts =
          job_class.start_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
      if (waits >= 200 && timeouts * 100 > waits &&
          !job_class.no_start_wait.exchange(true,
                                            std::memory_order_relaxed)) {
        XELOGW(
            "XDroid360: AC6 job start order skips class vt {:08X} ({} of {} "
            "waits timed out)",
            record->vt, timeouts, waits);
      }
      break;
    }
    // On a fiber this hands the dispatch thread to the next ready fiber,
    // possibly the worker itself.
    xe::kernel::GuestScheduler::SpinYield();
  }
}

void SiteHook(xe::cpu::ppc::PPCContext* ctx, void* arg0, void*) {
  const Site& site = *static_cast<const Site*>(arg0);
  if (site.after_bl) {
    AfterSubmit(site, ctx);
  }
  if (site.storage >= 0) {
    StorageBeforeSubmit(site, ctx);
  }
}

// Worker, right before bctrl job->vtbl[1](job, arg).
void WorkerStartHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  const uint32_t job = uint32_t(ctx->r[3]);
  if (!IsGuestHeap(job)) {
    return;
  }
  const uint32_t vt = Load32(job);
  if (!(vt >= 0x82000000u && vt < 0x82A00000u) ||
      Load32(vt + 4) != uint32_t(ctx->ctr)) {
    const uint32_t mismatches =
        g_start_mismatch.fetch_add(1, std::memory_order_relaxed) + 1;
    if (mismatches <= 4) {
      XELOGW(
          "XDroid360: AC6 job tracer: worker call target {:08X} is not "
          "vtbl[1] of r3 {:08X} ({} so far)",
          uint32_t(ctx->ctr), job, mismatches);
    }
    return;
  }
  // The oldest unstarted submit of this job: a job object resubmitted before
  // its first submit started (the effect-race shape) must match the first.
  const uint32_t last = g_seq.load(std::memory_order_acquire);
  Ac6JobRecord* record = nullptr;
  uint32_t unstarted = 0;
  for (uint32_t back = 0; back < 512 && back < last; ++back) {
    Ac6JobRecord* candidate = RecordFor(last - back);
    if (candidate && candidate->job == job &&
        !candidate->start_us.load(std::memory_order_relaxed)) {
      record = candidate;
      ++unstarted;
    }
  }
  if (unstarted > 1) {
    g_resubmits.fetch_add(1, std::memory_order_relaxed);
  }
  if (!record) {
    return;
  }
  const uint32_t seq = record->seq.load(std::memory_order_relaxed);
  const uint64_t now = Ac6NowUs();
  record->start_us.store(now, std::memory_order_release);
  g_thread_running[ThreadSlot(ctx)].store(seq, std::memory_order_relaxed);
  if (record->snap_state.exchange(3, std::memory_order_acq_rel) == 2) {
    uint64_t mask = 0;
    const uint8_t* current = g_membase + job;
    for (uint32_t word = 0; word * 4 < record->snapshot_len; ++word) {
      if (std::memcmp(current + word * 4, record->snapshot + word * 4, 4)) {
        mask |= uint64_t(1) << word;
        g_word_changes[word].fetch_add(1, std::memory_order_relaxed);
      }
    }
    g_diffs.fetch_add(1, std::memory_order_relaxed);
    record->post_store_mask = mask;
    record->post_store_valid = true;
    const uint64_t caller_stores = mask & ~BookkeepingMask();
    if (caller_stores) {
      Ac6JobClass& job_class = g_classes[record->cls];
      job_class.post_store_jobs.fetch_add(1, std::memory_order_relaxed);
      job_class.post_store_mask.fetch_or(caller_stores,
                                         std::memory_order_relaxed);
    }
  }
  if (auto on_start = g_on_start.load(std::memory_order_acquire)) {
    on_start(*record, now - record->submit_us);
  }
}

// Worker, right after the job returned.
void WorkerEndHook(xe::cpu::ppc::PPCContext* ctx, void*, void*) {
  const uint32_t seq = g_thread_running[ThreadSlot(ctx)].exchange(
      0, std::memory_order_relaxed);
  Ac6JobRecord* record = seq ? RecordFor(seq) : nullptr;
  if (!record) {
    return;
  }
  const uint64_t now = Ac6NowUs();
  // Release: the job's guest stores happen before the CP sees it ended.
  record->end_us.store(now, std::memory_order_release);
  const uint64_t start = record->start_us.load(std::memory_order_relaxed);
  Ac6JobClass& job_class = g_classes[record->cls];
  const uint32_t duration = uint32_t(std::min<uint64_t>(now - start, 1000000));
  const uint32_t ewma = job_class.duration_ewma_us.load(
      std::memory_order_relaxed);
  job_class.duration_ewma_us.store(
      ewma ? uint32_t(int64_t(ewma) + (int64_t(duration) - ewma) / 8)
           : duration,
      std::memory_order_relaxed);
}

void OnKick(uint32_t write_ptr) {
  const uint32_t index = g_kick_count.load(std::memory_order_relaxed);
  Kick& kick = g_kicks[index % kKickRing];
  kick.seq.store(g_seq.load(std::memory_order_acquire),
                 std::memory_order_relaxed);
  kick.write_ptr.store(write_ptr, std::memory_order_relaxed);
  g_kick_count.store(index + 1, std::memory_order_release);
}

// CP thread, before executing commands up to |write_ptr|.
void BeforeExecute(uint32_t write_ptr) {
  // H1: the CP parses frame |processed| (0-based); the guest finished it once
  // it has entered VdSwap more than |processed| times.
  if (cvars::xdroid_cp_defer_to_swap) {
    const uint32_t processed = xe::logging::GetFrameNumber();
    if (processed != g_defer_frame) {
      g_defer_frame = processed;
      g_defer_gave_up = false;
    }
    if (!g_defer_gave_up &&
        g_guest_swaps.load(std::memory_order_acquire) <= processed) {
      const uint64_t begin = Ac6NowUs();
      const uint64_t deadline =
          begin + uint64_t(std::max(cvars::xdroid_cp_defer_to_swap_us, 0));
      bool seen;
      while (!(seen = g_guest_swaps.load(std::memory_order_acquire) >
                      processed) &&
             Ac6NowUs() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      g_defer_gave_up = !seen;
      g_defer_wait[seen ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
      g_defer_wait[2].fetch_add(Ac6NowUs() - begin, std::memory_order_relaxed);
    }
  }
  const uint32_t kicks = g_kick_count.load(std::memory_order_acquire);
  if (!kicks) {
    return;
  }
  // The submit sequence when the guest kicked these commands: every job up to
  // it was submitted before the commands that may read its output.
  uint32_t seq = g_kicks[(kicks - 1) % kKickRing].seq.load(
      std::memory_order_relaxed);
  for (uint32_t back = 0; back < kKickRing && back < kicks; ++back) {
    const Kick& kick = g_kicks[(kicks - 1 - back) % kKickRing];
    if (kick.write_ptr.load(std::memory_order_relaxed) == write_ptr) {
      seq = kick.seq.load(std::memory_order_relaxed);
      break;
    }
  }
  const uint32_t frame = xe::logging::GetFrameNumber();
  if (frame != g_cp_frame) {
    g_cp_frame = frame;
    g_cp_frame_waited_us = 0;
  }
  const bool wait = cvars::xdroid_cp_wait_jobs;
  const uint64_t frame_budget =
      uint64_t(std::max(cvars::xdroid_cp_wait_jobs_frame_us, 0));
  for (uint32_t back = 0; back < 256 && back < seq; ++back) {
    Ac6JobRecord* record = RecordFor(seq - back);
    if (!record || record->end_us.load(std::memory_order_acquire)) {
      continue;
    }
    // Jobs of older frames are background work, not this frame's data.
    if (record->submit_frame + 2 < frame) {
      continue;
    }
    Ac6JobClass& job_class = g_classes[record->cls];
    if (job_class.storage.load(std::memory_order_relaxed)) {
      continue;
    }
    if (!record->cp_late.exchange(true, std::memory_order_relaxed)) {
      job_class.cp_late_jobs.fetch_add(1, std::memory_order_relaxed);
    }
    if (!wait || job_class.no_cp_wait.load(std::memory_order_relaxed) ||
        g_cp_frame_waited_us >= frame_budget) {
      continue;
    }
    if (job_class.duration_ewma_us.load(std::memory_order_relaxed) > 8000) {
      if (!job_class.no_cp_wait.exchange(true, std::memory_order_relaxed)) {
        XELOGI(
            "XDroid360: CP job wait skips class vt {:08X} (jobs run ~{} us)",
            record->vt,
            job_class.duration_ewma_us.load(std::memory_order_relaxed));
      }
      continue;
    }
    const uint64_t begin = Ac6NowUs();
    const uint64_t deadline = begin + (frame_budget - g_cp_frame_waited_us);
    const uint32_t record_seq = seq - back;
    bool ended = false;
    while (true) {
      if (record->end_us.load(std::memory_order_acquire) ||
          record->seq.load(std::memory_order_acquire) != record_seq) {
        ended = true;
        break;
      }
      if (Ac6NowUs() >= deadline) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    g_cp_frame_waited_us += Ac6NowUs() - begin;
    (ended ? job_class.cp_waits : job_class.cp_timeouts)
        .fetch_add(1, std::memory_order_relaxed);
  }
}

void OnGuestSwap() {
  TimeCouplingOnGuestSwap();
  const uint64_t swaps = g_guest_swaps.fetch_add(1, std::memory_order_relaxed);
  uint64_t processed = xe::logging::GetFrameNumber();
  uint64_t lag = swaps > processed ? swaps - processed : 0;
  g_swap_lag[std::min<uint64_t>(lag, 3)].fetch_add(1,
                                                   std::memory_order_relaxed);
  const uint64_t max_lag =
      uint64_t(std::max(cvars::xdroid_vdswap_wait_cp_max_lag, 0));
  if (!cvars::xdroid_vdswap_wait_cp || lag <= max_lag) {
    return;
  }
  const uint64_t begin = Ac6NowUs();
  const uint64_t deadline =
      begin + uint64_t(std::max(cvars::xdroid_vdswap_wait_cp_us, 0));
  bool caught_up;
  while (!(caught_up = swaps <= xe::logging::GetFrameNumber() + max_lag) &&
         Ac6NowUs() < deadline) {
    xe::kernel::GuestScheduler::SpinYield();
  }
  g_swap_wait[caught_up ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
  g_swap_wait[2].fetch_add(Ac6NowUs() - begin, std::memory_order_relaxed);
}

void OnGlitch(uint32_t frame) {
  if (auto on_glitch = g_on_glitch.load(std::memory_order_acquire)) {
    on_glitch(frame);
  }
}

}  // namespace

uint64_t Ac6NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

const Ac6JobRecord* Ac6JobRecordAt(uint32_t seq) { return RecordFor(seq); }

uint32_t Ac6JobLastSeq() { return g_seq.load(std::memory_order_acquire); }

uint64_t Ac6JobResubmits() {
  return g_resubmits.load(std::memory_order_relaxed);
}

Ac6JobClass* Ac6JobClassAt(uint32_t index) {
  if (index >= kAc6MaxClasses ||
      !g_classes[index].vt.load(std::memory_order_acquire)) {
    return nullptr;
  }
  return &g_classes[index];
}

void SetAc6JobObservers(const Ac6JobObservers& observers) {
  g_on_start.store(observers.on_start, std::memory_order_release);
  g_on_glitch.store(observers.on_glitch, std::memory_order_release);
}

void Ac6SwapLagHistogram(uint64_t out[4]) {
  for (int i = 0; i < 4; ++i) {
    out[i] = g_swap_lag[i].load(std::memory_order_relaxed);
  }
}

void Ac6CpDeferStats(uint64_t out[3]) {
  for (int i = 0; i < 3; ++i) {
    out[i] = g_defer_wait[i].load(std::memory_order_relaxed);
  }
}

void Ac6SwapWaitStats(uint64_t out[3]) {
  for (int i = 0; i < 3; ++i) {
    out[i] = g_swap_wait[i].load(std::memory_order_relaxed);
  }
}

void Ac6JobsOnSubmit(xe::cpu::ppc::PPCContext* ctx) {
  if (!g_membase) {
    return;
  }
  const uint32_t job = uint32_t(ctx->r[5]);
  if (!IsGuestHeap(job)) {
    return;
  }
  const uint32_t vt = Load32(job);
  if (!(vt >= 0x82000000u && vt < 0x82A00000u)) {
    return;
  }
  const uint32_t fn = Load32(vt + 4);
  const uint32_t seq = g_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  Ac6JobRecord& record = g_ring[seq % kAc6JobRing];
  record.seq.store(0, std::memory_order_release);
  record.job = job;
  record.vt = vt;
  record.fn = fn;
  const uint32_t lr = uint32_t(ctx->lr);
  record.site = IsCode(lr - 4) ? lr - 4 : 0;
  record.submit_frame = xe::logging::GetFrameNumber();
  record.cls = ClassFor(vt, fn);
  record.submit_us = Ac6NowUs();
  record.start_us.store(0, std::memory_order_relaxed);
  record.end_us.store(0, std::memory_order_relaxed);
  record.snap_state.store(0, std::memory_order_relaxed);
  record.post_store_mask = 0;
  record.post_store_valid = false;
  record.cp_late.store(false, std::memory_order_relaxed);
  // Within the job's page, at most 256 bytes.
  record.snapshot_len = std::min<uint32_t>(256, 0x1000 - (job & 0xFFF)) & ~3u;
  g_classes[record.cls].jobs.fetch_add(1, std::memory_order_relaxed);
  record.seq.store(seq, std::memory_order_release);
  g_thread_submit[ThreadSlot(ctx)].store(seq, std::memory_order_relaxed);
}

bool InstallAc6Jobs(xe::Emulator* emulator) {
  const uint8_t* membase = emulator->memory()->virtual_membase();
  auto module = emulator->kernel_state()->GetExecutableModule();
  const xe::cpu::XexModule* xex = module ? module->xex_module() : nullptr;
  if (!xex || xex->code_low_address() >= xex->code_high_address()) {
    XELOGW("XDroid360: AC6 job hooks not applied - no code range");
    return false;
  }
  const uint32_t low = xex->code_low_address();
  const uint32_t high = xex->code_high_address();
  auto bl_target = [membase](uint32_t address) -> uint32_t {
    const uint32_t code = xe::load_and_swap<uint32_t>(membase + address);
    if ((code & 0xFC000003u) != 0x48000001u) {  // bl, relative
      return 0;
    }
    int32_t offset = int32_t(code & 0x03FFFFFCu);
    if (offset & 0x02000000) {
      offset -= 0x04000000;
    }
    return address + uint32_t(offset);
  };
  bool layout_ok =
      kWorkerCall >= low && kWorkerReturn < high &&
      xe::load_and_swap<uint32_t>(membase + kWorkerCall) == kBctrl;
  for (const StorageSite& storage : kStorageSites) {
    layout_ok &= storage.bl >= low && storage.bl < high &&
                 bl_target(storage.bl) == kPoolSubmit;
  }
  if (!layout_ok) {
    XELOGW(
        "XDroid360: AC6 job hooks not applied - executable layout does not "
        "match");
    return false;
  }
  g_membase = membase;
  g_code_low = low;
  g_code_high = high;

  // Every bl Submit in the code section, and the storage ones among them.
  std::vector<uint32_t> calls;
  for (uint32_t address = low; address < high; address += 4) {
    if (bl_target(address) == kPoolSubmit) {
      calls.push_back(address);
    }
  }
  auto site_at = [](uint32_t address) -> Site& {
    for (auto& site : g_sites) {
      if (site->address == address) {
        return *site;
      }
    }
    g_sites.push_back(std::make_unique<Site>());
    g_sites.back()->address = address;
    return *g_sites.back();
  };
  for (uint32_t call : calls) {
    bool storage = false;
    for (size_t i = 0; i < kStorageSiteCount; ++i) {
      if (kStorageSites[i].bl == call) {
        site_at(call).storage = int(i);
        storage = true;
      }
    }
    Site& after = site_at(call + 4);
    after.after_bl = call;
    after.after_storage = storage;
  }
  xe::cpu::Processor* processor = emulator->processor();
  for (auto& site : g_sites) {
    processor->SetGuestHook(
        site->address,
        processor->DefineBuiltin("XDroidAc6JobSite", SiteHook, site.get(),
                                 nullptr),
        xe::cpu::Processor::GuestHookKind::kInstruction);
  }
  processor->SetGuestHook(
      kWorkerCall,
      processor->DefineBuiltin("XDroidAc6JobStart", WorkerStartHook, nullptr,
                               nullptr),
      xe::cpu::Processor::GuestHookKind::kInstruction);
  processor->SetGuestHook(
      kWorkerReturn,
      processor->DefineBuiltin("XDroidAc6JobEnd", WorkerEndHook, nullptr,
                               nullptr),
      xe::cpu::Processor::GuestHookKind::kInstruction);

  xe::gpu::XdroidGpuHooks& gpu_hooks = xe::gpu::GetXdroidGpuHooks();
  gpu_hooks.on_kick.store(OnKick, std::memory_order_release);
  gpu_hooks.before_execute.store(BeforeExecute, std::memory_order_release);
  gpu_hooks.on_guest_swap.store(OnGuestSwap, std::memory_order_release);
  gpu_hooks.on_glitch.store(OnGlitch, std::memory_order_release);
  XELOGI(
      "XDroid360: AC6 job hooks installed ({} Submit call sites, {} hooked "
      "addresses, code {:08X}-{:08X})",
      calls.size(), g_sites.size() + 2, low, high);
  return true;
}

}  // namespace xdroid
