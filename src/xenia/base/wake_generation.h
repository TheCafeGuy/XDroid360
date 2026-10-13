/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_WAKE_GENERATION_H_
#define XENIA_BASE_WAKE_GENERATION_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "xenia/base/platform.h"

#if XE_PLATFORM_LINUX
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <climits>
#include <ctime>
#endif  // XE_PLATFORM_LINUX

namespace xe {
namespace threading {

// A counter that producers bump and that a waiter can sleep on until it
// changes or a timeout passes. Lock-free on both sides (a futex on Linux and
// Android), so it may be used where taking a lock is forbidden, such as inside
// a guest-scheduler watchdog region. A missed wake only costs the timeout.
class WakeGeneration {
 public:
  uint32_t Load() const { return value_.load(std::memory_order_acquire); }

  void Bump() {
    // seq_cst on both sides (here, and the waiter's registration before its
    // re-check) so that either the waiter sees the new value or this sees the
    // waiter.
    value_.fetch_add(1, std::memory_order_seq_cst);
    if (waiters_.load(std::memory_order_seq_cst)) {
#if XE_PLATFORM_LINUX
      syscall(SYS_futex, reinterpret_cast<uint32_t*>(&value_),
              FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#endif  // XE_PLATFORM_LINUX
    }
  }

  // Returns once the value differs from `seen`, the timeout passes, or
  // spuriously.
  void WaitFor(uint32_t seen, std::chrono::microseconds timeout) {
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    if (value_.load(std::memory_order_seq_cst) == seen) {
#if XE_PLATFORM_LINUX
      timespec relative;
      relative.tv_sec = time_t(timeout.count() / 1000000);
      relative.tv_nsec = long(timeout.count() % 1000000) * 1000;
      syscall(SYS_futex, reinterpret_cast<uint32_t*>(&value_),
              FUTEX_WAIT_PRIVATE, seen, &relative, nullptr, 0);
#else
      std::this_thread::sleep_for(timeout);
#endif  // XE_PLATFORM_LINUX
    }
    waiters_.fetch_sub(1, std::memory_order_relaxed);
  }

 private:
  static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
                "The futex word must be the atomic itself");
  std::atomic<uint32_t> value_{0};
  std::atomic<uint32_t> waiters_{0};
};

// Bumped by the GPU command processor after every write-back to guest memory
// that a guest thread may be polling (fences, the ring read pointer), so a
// guest spin-wait can sleep until one happens instead of spinning (CE-7).
inline WakeGeneration& GuestGpuWritebackGeneration() {
  static WakeGeneration generation;
  return generation;
}

}  // namespace threading
}  // namespace xe

#endif  // XENIA_BASE_WAKE_GENERATION_H_
