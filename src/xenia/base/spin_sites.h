/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_SPIN_SITES_H_
#define XENIA_BASE_SPIN_SITES_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace xe {
namespace threading {

// XDroid360 CE-7b: statistics of guest spin-waits (db16cyc) per site, keyed by
// the guest address of a spin's first hint. A spin counts as released by the
// GPU when a GPU write-back happened between its first and last hint.
// guest_spin_park=3/4 park only at sites the GPU releases, so spins that wait
// on another guest thread keep their exact timing (park=1 exposed a guest
// race in menus, DONE_LOG 2026-10-12). Filled lock-free from guest threads,
// possibly inside a scheduler watchdog region (hard rule 8); counters are
// cumulative for the process.
struct SpinSite {
  std::atomic<uint32_t> pc{0};
  std::atomic<uint32_t> spins{0};
  std::atomic<uint32_t> gpu_released{0};
  std::atomic<uint32_t> parks{0};
  std::atomic<uint64_t> spin_ns{0};
  // Recent share of spins the GPU ended, in 1/65536 (moving average over about
  // the last 32 spins). The cumulative share hides scene changes: AC6's fence
  // sled is GPU-released ~80% in the intro, ~35% on the title screen.
  std::atomic<uint32_t> recent_gpu{0};

  // Called once per finished spin. Racy read-modify-write between threads
  // sharing a site only blurs the average.
  void RecordSpin(bool released_by_gpu) {
    spins.fetch_add(1, std::memory_order_relaxed);
    if (released_by_gpu) {
      gpu_released.fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t average = recent_gpu.load(std::memory_order_relaxed);
    average -= average >> 5;
    if (released_by_gpu) {
      average += 65536u >> 5;
    }
    recent_gpu.store(average, std::memory_order_relaxed);
  }

  // Learned: enough spins, and the GPU ended at least |percent| of the recent
  // ones.
  bool GpuReleased(uint32_t percent) const {
    return spins.load(std::memory_order_relaxed) >= 64 &&
           uint64_t(recent_gpu.load(std::memory_order_relaxed)) * 100 >=
               uint64_t(percent) * 65536;
  }
};

constexpr size_t kMaxSpinSites = 64;

inline SpinSite* SpinSites() {
  static SpinSite sites[kMaxSpinSites];
  return sites;
}

// The site's slot, claimed with a CAS on first use; null when the table is
// full or pc is 0.
inline SpinSite* FindSpinSite(uint32_t pc) {
  if (!pc) {
    return nullptr;
  }
  SpinSite* sites = SpinSites();
  size_t index = size_t((pc >> 2) * 2654435761u) % kMaxSpinSites;
  for (size_t probe = 0; probe < kMaxSpinSites; ++probe) {
    SpinSite& site = sites[index];
    uint32_t current = site.pc.load(std::memory_order_acquire);
    if (current == 0) {
      if (site.pc.compare_exchange_strong(current, pc,
                                          std::memory_order_acq_rel)) {
        return &site;
      }
    }
    if (current == pc) {
      return &site;
    }
    index = (index + 1) % kMaxSpinSites;
  }
  return nullptr;
}

}  // namespace threading
}  // namespace xe

#endif  // XENIA_BASE_SPIN_SITES_H_
