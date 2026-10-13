/**
 * XDroid360: Ace Combat 6 job pool B ordering (HUD-1, race 2).
 */

#ifndef XDROID_AC6_JOBS_H_
#define XDROID_AC6_JOBS_H_

#include <atomic>
#include <cstdint>

#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
class Emulator;
}  // namespace xe

namespace xdroid {

// Registers the job pool hooks (AC6 only). Call from Emulator::on_launch,
// before guest code is translated. Returns false if the executable layout does
// not match, in which case nothing is hooked.
bool InstallAc6Jobs(xe::Emulator* emulator);

// Called by the pool Submit entry hook (xdroid_ac6_fixes.cc owns that address).
void Ac6JobsOnSubmit(xe::cpu::ppc::PPCContext* ctx);

// Lifecycle of one pool B job, in a ring indexed by submit sequence. Written by
// the submitting guest thread (fields, then |seq| with release), the worker
// (|start_us|, |end_us| with release) and the CP thread (|cp_late|).
struct Ac6JobRecord {
  std::atomic<uint32_t> seq{0};
  uint32_t job = 0;
  uint32_t vt = 0;
  uint32_t fn = 0;        // vtbl[1], what the worker calls
  uint32_t site = 0;      // bl address, 0 if called through a pointer
  uint32_t submit_frame = 0;
  uint32_t cls = 0;       // Ac6JobClassAt index
  uint64_t submit_us = 0;
  std::atomic<uint64_t> start_us{0};
  std::atomic<uint64_t> end_us{0};
  // 0 submitted, 1 caller snapshot being taken, 2 snapshot ready, 3 started.
  std::atomic<uint32_t> snap_state{0};
  // Words of the job changed between Submit returning and the job start (the
  // caller stored after Submit: race 2 class), bit per 4-byte word.
  uint64_t post_store_mask = 0;
  bool post_store_valid = false;
  // The CP executed commands kicked after this submit before the job ended.
  std::atomic<bool> cp_late{false};
  uint8_t snapshot[256];
  uint32_t snapshot_len = 0;
};

constexpr uint32_t kAc6JobRing = 1024;
const Ac6JobRecord* Ac6JobRecordAt(uint32_t seq);
uint32_t Ac6JobLastSeq();
// Job starts that found the same object submitted twice, still unstarted.
uint64_t Ac6JobResubmits();

// Per job class (vtable) counters, at most kAc6MaxClasses classes.
struct Ac6JobClass {
  std::atomic<uint32_t> vt{0};
  std::atomic<uint32_t> fn{0};
  std::atomic<uint64_t> jobs{0};
  std::atomic<uint64_t> post_store_jobs{0};  // race 2 class evidence
  std::atomic<uint64_t> post_store_mask{0};
  std::atomic<uint64_t> cp_late_jobs{0};
  std::atomic<uint64_t> start_waits{0};      // arm A waits
  std::atomic<uint64_t> start_timeouts{0};
  std::atomic<uint64_t> cp_waits{0};         // arm C+ waits on this class
  std::atomic<uint64_t> cp_timeouts{0};
  std::atomic<uint32_t> duration_ewma_us{0};
  std::atomic<bool> no_start_wait{false};    // excluded from arm A
  std::atomic<bool> no_cp_wait{false};       // excluded from arm C+
  std::atomic<bool> storage{false};          // submitted by a storage site
};
constexpr uint32_t kAc6MaxClasses = 64;
Ac6JobClass* Ac6JobClassAt(uint32_t index);  // nullptr past the last one

// Optional observers for the forensic tracer (temp).
struct Ac6JobObservers {
  void (*on_start)(const Ac6JobRecord& record, uint64_t latency_us) = nullptr;
  void (*on_glitch)(uint32_t frame) = nullptr;
};
void SetAc6JobObservers(const Ac6JobObservers& observers);

uint64_t Ac6NowUs();

// Guest VdSwap calls by how many swaps the CP was still behind at that moment
// (0, 1, 2, 3+), since launch.
void Ac6SwapLagHistogram(uint64_t out[4]);
// xdroid_vdswap_wait_cp totals: waits that caught up, timeouts, waited us.
void Ac6SwapWaitStats(uint64_t out[3]);
// xdroid_cp_defer_to_swap (H1) totals: waits that saw the swap, timeouts, us.
void Ac6CpDeferStats(uint64_t out[3]);

// Forensic (HUD-1, temp): xdroid_ac6_jobs_trace.cc.
void InstallAc6JobTracer();

}  // namespace xdroid

#endif  // XDROID_AC6_JOBS_H_
