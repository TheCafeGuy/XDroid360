/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XEVENT_H_
#define XENIA_KERNEL_XEVENT_H_

#include <mutex>

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

// https://www.nirsoft.net/kernel_struct/vista/KEVENT.html
struct X_KEVENT {
  X_DISPATCH_HEADER header;
};
static_assert_size(X_KEVENT, 0x10);

class XEvent : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Event;

  explicit XEvent(KernelState* kernel_state, bool host_object = false);
  ~XEvent() override;

  void Initialize(bool manual_reset, bool initial_state);
  void InitializeNative(void* native_ptr, const X_DISPATCH_HEADER* header);

  int32_t Set(uint32_t priority_increment, bool wait);
  int32_t Pulse(uint32_t priority_increment, bool wait);
  int32_t Reset();
  void Query(uint32_t* out_type, uint32_t* out_state);
  void Clear();

  bool Save(ByteStream* stream) override;
  static object_ref<XEvent> Restore(KernelState* kernel_state,
                                    ByteStream* stream);

  uint32_t cooperative_pulse_epoch() const override {
    return pulse_epoch_.load();
  }
  // A fiber in a cooperative WaitAny holding this event (see Pulse).
  void AddMultiWaiter() { multi_waiters_.fetch_add(1); }
  void RemoveMultiWaiter() { multi_waiters_.fetch_sub(1); }

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override { return event_.get(); }
  void WaitCallback() override;
  void SyncFromGuest() override;

  void CooperativeWaitBegin(XThread* thread) override;
  void CooperativeWaitEnd(XThread* thread) override;
  bool CooperativeMayAcquire(XThread* thread) override;

 private:
  bool manual_reset_ = false;
  std::unique_ptr<xe::threading::Event> event_;
  // Guards the guest header, the mirror below and the host event together, so
  // a reconcile never samples a kernel write half done.
  std::mutex state_lock_;
  // Last signal_state this kernel wrote, so SyncFromGuest can tell a guest
  // write from one of ours. Guarded by state_lock_.
  bool host_signaled_ = false;
  // Parked cooperative waiters, so Pulse knows one will consume a set.
  CooperativeWaiterFifo waiters_;
  std::atomic<uint32_t> pulse_epoch_{0};
  // Fibers polling this event inside a cooperative WaitAny. They never park
  // on waiters_, so without this count Pulse sees no waiter and drops it.
  std::atomic<uint32_t> multi_waiters_{0};
};

// Counts a cooperative WaitAny fiber as a waiter on each event it holds, for
// the life of the wait.
class XEventMultiWaitScope {
 public:
  XEventMultiWaitScope(XObject** objects, uint32_t count)
      : objects_(objects), count_(count) {
    Each(&XEvent::AddMultiWaiter);
  }
  ~XEventMultiWaitScope() { Each(&XEvent::RemoveMultiWaiter); }

 private:
  void Each(void (XEvent::*op)()) {
    for (uint32_t i = 0; objects_ && i < count_; ++i) {
      if (objects_[i]->type() == XObject::Type::Event) {
        (static_cast<XEvent*>(objects_[i])->*op)();
      }
    }
  }
  XObject** objects_;
  uint32_t count_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XEVENT_H_
