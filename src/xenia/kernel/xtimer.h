/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XTIMER_H_
#define XENIA_KERNEL_XTIMER_H_

#include <mutex>

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class XThread;

class XTimer : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Timer;

  explicit XTimer(KernelState* kernel_state);
  ~XTimer() override;

  void Initialize(uint32_t timer_type);

  X_STATUS SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                    uint32_t routine_arg, bool resume);
  X_STATUS Cancel();

  // Disarms every timer so none fires into a kernel being torn down.
  static void CancelAll();

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override {
    if (signal_) {
      return signal_.get();
    }
    return timer_.get();
  }

 private:
  // Callers must cancel the host timer first. Returns the APC thread's
  // reference: drop it outside timer_lock_, as the last release of an exited
  // thread takes the global lock, which CancelAll holds before timer_lock_.
  object_ref<XThread> RemoveApc();

  std::unique_ptr<xe::threading::Timer> timer_;
  // Guest scheduler only. Set by the expiry callback, so a waiter it wakes
  // finds it signaled.
  std::unique_ptr<xe::threading::Event> signal_;
  std::mutex timer_lock_;

  // Reused across expiries like the KAPC a KTIMER embeds.
  uint32_t apc_ptr_ = 0;
  object_ref<XThread> apc_thread_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTIMER_H_
