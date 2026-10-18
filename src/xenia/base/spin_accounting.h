/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_SPIN_ACCOUNTING_H_
#define XENIA_BASE_SPIN_ACCOUNTING_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "xenia/base/platform.h"

#if XE_PLATFORM_LINUX
#include <sys/syscall.h>
#include <unistd.h>
#endif  // XE_PLATFORM_LINUX

namespace xe {
namespace threading {

// XDroid360 (ADPF-1): CPU time each host thread burns in guest spin-waits, so
// the ADPF performance hint can report a frame's real work instead of the time
// a spinning thread waits for the GPU. Filled by the db16cyc helper
// (GuestSpinPark, emitted when guest_spin_park != 0) only while Enabled() is
// set; read by the ADPF thread by host thread id.
struct SpinAccount {
  std::atomic<int32_t> tid{0};
  std::atomic<uint64_t> spin_ns{0};
};

constexpr size_t kMaxSpinAccounts = 32;

inline SpinAccount* SpinAccounts() {
  static SpinAccount accounts[kMaxSpinAccounts];
  return accounts;
}

inline std::atomic<bool>& SpinAccountingEnabled() {
  static std::atomic<bool> enabled{false};
  return enabled;
}

// This thread's slot, registered on first use; null if the table is full or
// the platform has no thread ids.
inline SpinAccount* ThisThreadSpinAccount() {
  thread_local SpinAccount* account = nullptr;
  thread_local bool looked_up = false;
  if (looked_up) {
    return account;
  }
  looked_up = true;
#if XE_PLATFORM_LINUX
  // Lock-free (a CAS claims a free slot): the caller may be inside a guest
  // scheduler watchdog region, where taking a lock is forbidden.
  const int32_t tid = int32_t(syscall(SYS_gettid));
  SpinAccount* accounts = SpinAccounts();
  for (size_t i = 0; i < kMaxSpinAccounts; ++i) {
    int32_t slot_tid = accounts[i].tid.load(std::memory_order_acquire);
    if (slot_tid == 0 && accounts[i].tid.compare_exchange_strong(
                             slot_tid, tid, std::memory_order_acq_rel)) {
      slot_tid = tid;
    }
    if (slot_tid == tid) {
      account = &accounts[i];
      break;
    }
  }
#endif  // XE_PLATFORM_LINUX
  return account;
}

// Spin nanoseconds charged so far to host thread `tid` (0 if none).
inline uint64_t SpinNanosecondsForThread(int32_t tid) {
  SpinAccount* accounts = SpinAccounts();
  for (size_t i = 0; i < kMaxSpinAccounts; ++i) {
    if (accounts[i].tid.load(std::memory_order_acquire) == tid) {
      return accounts[i].spin_ns.load(std::memory_order_relaxed);
    }
  }
  return 0;
}

}  // namespace threading
}  // namespace xe

#endif  // XENIA_BASE_SPIN_ACCOUNTING_H_
