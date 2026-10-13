/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"

// Port of edge 54729c29f (Herman S.), gated until an A/B shows its effect on
// timer wake cadence and energy (PES-1).
DEFINE_bool(timer_console_tick, false,
            "Expire guest timers on the console's 1 ms clock tick (edge "
            "54729c29f). A relative due time counts from the last tick and an "
            "expiry rounds up to a tick.",
            "Kernel");

namespace xe {
namespace kernel {

namespace {
// Leaked so they outlive static destruction.
std::mutex& LiveTimersLock() {
  static auto* lock = new std::mutex;
  return *lock;
}
std::vector<XTimer*>& LiveTimers() {
  static auto* timers = new std::vector<XTimer*>;
  return *timers;
}
}  // namespace

XTimer::XTimer(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {
  std::lock_guard<std::mutex> lock(LiveTimersLock());
  LiveTimers().push_back(this);
}

XTimer::~XTimer() {
  {
    std::lock_guard<std::mutex> lock(LiveTimersLock());
    auto& timers = LiveTimers();
    timers.erase(std::remove(timers.begin(), timers.end(), this), timers.end());
  }
  if (timer_) {
    timer_->Cancel();
  }
  RemoveApc();
  memory()->SystemHeapFree(apc_ptr_);
}

void XTimer::Initialize(uint32_t timer_type) {
  // CancelAll can already see this timer.
  std::lock_guard<std::mutex> lock(timer_lock_);
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      if (GuestScheduler::enabled()) {
        signal_ = xe::threading::Event::CreateManualResetEvent(false);
      }
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      if (GuestScheduler::enabled()) {
        signal_ = xe::threading::Event::CreateAutoResetEvent(false);
      }
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

void XTimer::CancelAll() {
  // Global lock first, as ~XTimer is entered holding it.
  auto global_lock = xe::global_critical_region::AcquireDirect();
  std::lock_guard<std::mutex> lock(LiveTimersLock());
  for (XTimer* timer : LiveTimers()) {
    timer->Cancel();
  }
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;

  // Declared before the lock so it is released after it.
  object_ref<XThread> old_apc_thread;
  std::lock_guard<std::mutex> lock(timer_lock_);

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (cvars::timer_console_tick) {
    // The console expires timers only on its 1 ms clock tick and counts a
    // relative due time from the last tick. An expiry rounds up to a tick and
    // one already due at the last tick fires at once.
    constexpr uint64_t kClockTick = 10000;
    const uint64_t now = XSystemClock::to_file_time(XSystemClock::now());
    const uint64_t last_tick = now - now % kClockTick;
    const uint64_t due = due_time < 0
                             ? last_tick + (0 - static_cast<uint64_t>(due_time))
                             : static_cast<uint64_t>(due_time);
    uint64_t expiry = now;
    if (due > last_tick) {
      expiry = (due + kClockTick - 1) / kClockTick * kClockTick;
    }
    due_tp =
        date::clock_cast<WinSystemClock>(XSystemClock::from_file_time(expiry));
  } else if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = xe::chrono::hundrednanoseconds(-due_time);
    due_tp = date::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = date::clock_cast<WinSystemClock>(
        XSystemClock::from_file_time(due_time));
  }

  // An absolute due time of 0 means fire now. Clamp past times before the
  // host timers convert them.
  auto now_wsc = WinSystemClock::now();
  if (due_tp < now_wsc) {
    due_tp = now_wsc;
  }

  // The previous expiry must not be able to queue the APC while it is reused.
  timer_->Cancel();
  old_apc_thread = RemoveApc();
  if (signal_) {
    // KeSetTimer clears the signal state.
    signal_->Reset();
  }

  // This callback will only be issued when the timer is fired.
  // Capture values by value to avoid racing with a future SetTimer() call.
  XThread* cb_thread = nullptr;
  uint32_t cb_apc = 0;
  if (routine) {
    if (!apc_ptr_) {
      apc_ptr_ = memory()->SystemHeapAlloc(XAPC::kSize);
      if (!apc_ptr_) {
        return X_STATUS_NO_MEMORY;
      }
    }
    apc_thread_ = retain_object(XThread::GetCurrentThread());
    xboxkrnl::xeKeInitializeApc(memory()->TranslateVirtual<XAPC*>(apc_ptr_),
                                apc_thread_->guest_object(),
                                XAPC::kOwnedKernelRoutine, 0, routine, 1,
                                routine_arg);
    cb_thread = apc_thread_.get();
    cb_apc = apc_ptr_;
  }
  std::function<void()> callback = nullptr;
  if (routine || signal_) {
    // Signal and unwait cooperative waiters, as the timer DPC does.
    xe::threading::Event* signal = signal_.get();
    callback = [this, signal, cb_thread, cb_apc, routine, routine_arg]() {
      if (signal) {
        signal->Set();
      }
      if (cb_thread) {
        // Queue APC to call back routine with (arg, low, high).
        // It'll be executed on the thread that requested the timer.
        uint64_t time = xe::Clock::QueryGuestSystemTime();
        uint32_t time_low = static_cast<uint32_t>(time);
        uint32_t time_high = static_cast<uint32_t>(time >> 32);
        XELOGD(
            "XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, "
            "{:08X})",
            routine, routine_arg, time_low, time_high);
        cb_thread->InsertOwnedApc(cb_apc, time_low, time_high);
      }
      if (signal) {
        WakeCooperativeWaiters();
      }
    };
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result = timer_->SetRepeatingAt(
        due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  object_ref<XThread> old_apc_thread;
  std::lock_guard<std::mutex> lock(timer_lock_);
  if (!timer_) {
    return X_STATUS_UNSUCCESSFUL;
  }
  bool result = timer_->Cancel();
  old_apc_thread = RemoveApc();
  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

object_ref<XThread> XTimer::RemoveApc() {
  if (apc_thread_) {
    apc_thread_->RemoveOwnedApc(apc_ptr_);
  }
  return std::move(apc_thread_);
}

}  // namespace kernel
}  // namespace xe
