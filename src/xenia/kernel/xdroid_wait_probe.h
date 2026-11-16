/**
 * XDroid360 forensic (AUD-2, gated by xdroid_diagnostics): attributes the time
 * a host thread spends in guest kernel waits to (guest caller, wait kind), for
 * a thread that installs a probe (the audio worker, around the guest audio
 * callback).
 */

#ifndef XENIA_KERNEL_XDROID_WAIT_PROBE_H_
#define XENIA_KERNEL_XDROID_WAIT_PROBE_H_

#include <atomic>
#include <chrono>
#include <cstdint>

#include "xenia/base/cvar.h"

DECLARE_bool(xdroid_diagnostics);
DECLARE_bool(xdroid_audio_producer_boost);
DECLARE_int32(xdroid_audio_producer_nice);
DECLARE_int32(xdroid_adpf_exclude);

namespace xe {
namespace kernel {

// AUD-2/AUD-5 split (syscalls per producer slice and per callback): read per
// use, so it follows a live toggle.
inline bool XdroidAudioForensics() { return cvars::xdroid_diagnostics; }
// Signaler discovery (the worker's wait probe) feeds the boost and nice arms
// and the ADPF exclusion (AUDIO-H1, learns the producer from signaler_tid), so
// it stays on while any of them is armed.
inline bool XdroidAudioProbeWanted() {
  return XdroidAudioForensics() || cvars::xdroid_audio_producer_boost ||
         cvars::xdroid_audio_producer_nice != 0 ||
         cvars::xdroid_adpf_exclude != 0;
}

struct XdroidWaitProbe {
  struct Entry {
    uint32_t lr = 0;
    uint32_t kind = 0;  // XObject::Type, or 100 WaitMultiple, 101 KeDelay,
                        // 102 SignalAndWait
    uint32_t count = 0;
    uint64_t total_us = 0;
  };
  static constexpr int kEntries = 16;
  Entry entries[kEntries];
  uint64_t total_us = 0;
  void Record(uint32_t lr, uint32_t kind, uint64_t us) {
    total_us += us;
    Entry* slot = nullptr;
    for (Entry& entry : entries) {
      if (entry.count && entry.lr == lr && entry.kind == kind) {
        slot = &entry;
        break;
      }
      if (!entry.count && !slot) {
        slot = &entry;
      }
    }
    if (!slot) {
      slot = &entries[kEntries - 1];  // Full: lumped into the last one.
    }
    if (!slot->count) {
      slot->lr = lr;
      slot->kind = kind;
    }
    ++slot->count;
    slot->total_us += us;
  }
  void Reset() { *this = XdroidWaitProbe(); }
};

// Set by the thread that wants its waits attributed; null otherwise.
inline thread_local XdroidWaitProbe* xdroid_wait_probe = nullptr;

// Guest LR of the current thread's caller (defined in xobject.cc).
uint32_t XdroidCurrentGuestLr();

// The objects the probed thread is waiting on right now, and who signals them
// (AUD-2: the guest audio callback blocks in one WaitMultiple at 823ADA64).
// Producer states at wait start: 0 running, 1 ready (waiting for a CPU),
// 2 blocked, 3 unknown.
struct XdroidAudioWatch {
  static constexpr int kObjects = 4;
  std::atomic<const void*> objects[kObjects] = {};
  std::atomic<uint64_t> wait_start_us{0};
  std::atomic<uint32_t> signaler_tid{0};
  std::atomic<uint32_t> signaler_lr{0};
  std::atomic<uint64_t> signals{0};
  std::atomic<uint64_t> signal_latency_us{0};
  std::atomic<uint64_t> producer_state[4] = {};
  std::atomic<uint64_t> boosts{0};
  // The producer is usually blocked on the event the callback signals right
  // before its wait (a ping-pong), so the boost applies when the scheduler
  // makes it ready: its next ready link goes to the head of the list.
  std::atomic<uint32_t> boost_pending_tid{0};
  // Producer wake-to-dispatch delay (ready link to fiber switch), diagnostics
  // only.
  std::atomic<uint64_t> producer_ready_tick{0};
  std::atomic<uint64_t> dispatch_delay_us{0};
  std::atomic<uint64_t> dispatches{0};
  std::atomic<uint64_t> dispatch_delay_max_us{0};
  // AUD-5: the producer's own slices on its dispatch host thread, from
  // switch-in to switch-out: wall time, host CPU time, host run-queue delay
  // (schedstat), and why each slice ended (wait, preempted, quantum end,
  // voluntary yield such as a contended spinlock). Thread 18 (AC6's XAudio2
  // render thread) never waits on another guest object while it renders, so its
  // render time is CPU, host scheduling or yields; this splits them.
  std::atomic<uint64_t> slices{0};
  std::atomic<uint64_t> slice_wall_us{0};
  std::atomic<uint64_t> slice_cpu_us{0};
  std::atomic<uint64_t> slice_runq_us{0};
  std::atomic<uint64_t> slice_end_wait{0};
  std::atomic<uint64_t> slice_end_preempt{0};
  std::atomic<uint64_t> slice_end_quantum{0};
  std::atomic<uint64_t> slice_end_yield{0};
  // Producer CPU time per host core it was dispatched on (core index < 16).
  static constexpr int kHostCpus = 16;
  std::atomic<uint64_t> host_cpu_us[kHostCpus] = {};
  // Nice of the dispatch thread at the last switch-in (before any boost).
  std::atomic<int32_t> dispatch_nice{0};
  // Bumped by the audio pump at each 10 s report: the dispatch thread
  // re-reads its nice once per window instead of once per slice.
  std::atomic<uint32_t> window{0};
  // F2: longest guest audio callback (us) since the AAudio driver's last audio
  // report line took it.
  std::atomic<uint64_t> pump_took_max_us{0};
  // Set by the AAudio driver after a long stretch of exact-zero guest output;
  // the scheduler watchdog takes it and dumps every guest fiber's wait
  // (2026-11-13: AC6 boots without music 2 of ~36 times).
  std::atomic<const char*> sched_dump_reason{nullptr};
};
inline XdroidAudioWatch xdroid_audio_watch;
// The audio worker after each guest audio callback. Lock-free: the driver
// takes the value from its real-time callback.
inline void XdroidNotePumpCall(uint64_t took_us) {
  auto& max_us = xdroid_audio_watch.pump_took_max_us;
  uint64_t seen = max_us.load(std::memory_order_relaxed);
  while (took_us > seen && !max_us.compare_exchange_weak(
                               seen, took_us, std::memory_order_relaxed)) {
  }
}
inline uint64_t XdroidNowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

class XdroidScopedWait {
 public:
  explicit XdroidScopedWait(uint32_t kind)
      : probe_(xdroid_wait_probe), kind_(kind) {
    if (probe_) {
      start_ = std::chrono::steady_clock::now();
    }
  }
  ~XdroidScopedWait() {
    if (probe_) {
      probe_->Record(
          XdroidCurrentGuestLr(), kind_,
          uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - start_)
                       .count()));
    }
  }

 private:
  XdroidWaitProbe* probe_;
  uint32_t kind_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XDROID_WAIT_PROBE_H_
