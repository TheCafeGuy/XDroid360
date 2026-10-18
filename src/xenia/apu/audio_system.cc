/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_system.h"

#include <algorithm>
#include <chrono>

#include <string>
#include <vector>

#include <limits>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xdroid_wait_probe.h"

#if XE_PLATFORM_ANDROID
#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#endif

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

// XDroid360: 16 (85 ms) rather than 8. Guest mixer responses on Android
// stall 20-77 ms now and then under load (measured 2026-10-07, AC6), which
// emptied an 8-frame queue and was heard as dropouts; 16 absorbs them.
DEFINE_uint32(apu_max_queued_frames, 16,
              "Allows changing max buffered audio frames to reduce audio "
              "delay. Lowering this value might cause performance issues. "
              "Value range: [4-64]",
              "APU");
UPDATE_from_uint32(apu_max_queued_frames, 2024, 8, 31, 20, 64);
// Configs saved with the old default of 8 move to 16.
UPDATE_from_uint32(apu_max_queued_frames, 2026, 10, 9, 13, 8);

// XDroid360: pace the pump from the output's consumption instead of a
// timer. The driver releases a client's semaphore each time the device
// consumes a frame, so waiting on it yields one guest callback per consumed
// frame: no clock drift between the timer and the DAC, no slots lost when a
// pump runs late, and no precise-sleep spinning. Drivers that release slots in
// large bursts can turn it off to get the timer-paced pump.
DEFINE_bool(apu_consumer_paced, true,
            "Run the guest audio callback each time the output consumes a "
            "frame (no pump timer).",
            "APU");

DEFINE_int32(xdroid_audio_worker_nice, 0,
             "XDroid360 (AUD-2 arm D1): nice value for the audio worker thread "
             "that runs the guest audio callback (0 = leave it; -16 to -19 is "
             "Android's audio class). Applied live by the worker, read back "
             "and logged.",
             "XDroid360");

DECLARE_int32(xdroid_audio_producer_nice);

namespace xe {
namespace apu {

#if XE_PLATFORM_ANDROID
namespace {
// XDroid360 AUD-2: where the guest audio callback's wall time goes, from the
// calling thread's own counters: CPU time, time runnable but waiting for a
// core (schedstat run delay), and the rest (blocked or sleeping).
struct CallbackSplit {
  uint64_t cpu_ns = 0;
  uint64_t runq_ns = 0;
};
CallbackSplit ReadCallbackSplit() {
  static thread_local int fd = -2;
  if (fd == -2) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat",
                  int(gettid()));
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      XELOGW("XDroid360: audio pump: no {} - run delay not measured", path);
    }
  }
  CallbackSplit split;
  timespec ts;
  if (!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) {
    split.cpu_ns = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
  }
  if (fd >= 0) {
    char text[96];
    ssize_t length = pread(fd, text, sizeof(text) - 1, 0);
    if (length > 0) {
      text[length] = '\0';
      char* end = nullptr;
      std::strtoull(text, &end, 10);  // On-CPU time, same as above.
      split.runq_ns = std::strtoull(end, nullptr, 10);
    }
  }
  return split;
}
// Applies xdroid_audio_worker_nice to the calling thread when it changes.
void ApplyWorkerNice() {
  static thread_local int32_t applied = 0;
  static thread_local bool logged = false;
  const int32_t wanted = cvars::xdroid_audio_worker_nice;
  if (wanted == applied && logged) {
    return;
  }
  if (wanted != applied) {
    if (setpriority(PRIO_PROCESS, 0, wanted)) {
      XELOGW("XDroid360: audio worker nice {} refused (errno {})", wanted,
             errno);
    }
    applied = wanted;
  }
  logged = true;
  errno = 0;
  XELOGI("XDroid360: audio worker nice now {} (asked {})",
         getpriority(PRIO_PROCESS, 0), wanted);
}
}  // namespace
#endif

AudioSystem::AudioSystem(cpu::Processor* processor)
    : memory_(processor->memory()),
      processor_(processor),
      worker_running_(false) {
  queued_frames_ = std::clamp(cvars::apu_max_queued_frames,
                              static_cast<uint32_t>(kMinimumQueuedFrames),
                              static_cast<uint32_t>(kMaximumQueuedFrames));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = xe::threading::Semaphore::Create(0, queued_frames_);
  }
  pending_work_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(pending_work_event_);

  xma_decoder_ = std::make_unique<xe::apu::XmaDecoder>(processor_);

  resume_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(kernel::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state, 128 * 1024, 0,
          [this]() {
            WorkerThreadMain();
            return 0;
          },
          kernel_state->GetSystemProcess()));
  // As we run audio callbacks the debugger must be able to suspend us.
  worker_thread_->set_can_debugger_suspend(true);
  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();
  // Set high priority for this thread for better pacing.
  worker_thread_->SetPriority(24);
  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();

  if (cvars::apu_consumer_paced) {
    ConsumerPacedWorkerLoop();
    worker_running_ = false;
    return;
  }

  // The host mixer releases a client's semaphore on its own coarse cadence,
  // but Xenos audio subsystem operates at 5.333ms interval (see
  // xaudio2_audio_driver.cc) Interval scales inversely with guest_time_scalar.
  // We therefore pace pumps to each client's next_pump_us deadline and use
  // the semaphore only as back-pressure: a frame is submitted only if
  // a host output slot is free, otherwise it is dropped (the host queue
  // is full, so it is already well buffered).
  while (worker_running_) {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    size_t client_index = kMaximumClientCount;
    uint64_t earliest_pump_us = std::numeric_limits<uint64_t>::max();
    {
      std::lock_guard<std::mutex> clients_lock(clients_mutex_);

      for (size_t i = 0; i < kMaximumClientCount; ++i) {
        if (!clients_[i].in_use ||
            clients_[i].next_pump_us >= earliest_pump_us) {
          continue;
        }
        earliest_pump_us = clients_[i].next_pump_us;
        client_index = i;
      }

      if (client_index != kMaximumClientCount) {
        const double scalar = xe::Clock::guest_time_scalar();
        const uint64_t min_us =
            scalar > 0.0 ? static_cast<uint64_t>(kAudioPumpInterval / scalar)
                         : kAudioPumpInterval;
        // Counted from the deadline, so a late pump is made up and the host
        // queue absorbs the stall, until it falls further behind than the
        // queue holds.
        uint64_t next_pump_us = earliest_pump_us + min_us;
        if (now > earliest_pump_us + min_us * queued_frames_) {
          next_pump_us = now + min_us;
        }
        clients_[client_index].next_pump_us = next_pump_us;
      }
    }

    // No clients yet: park until one registers or we're told to stop.
    if (client_index == kMaximumClientCount) {
      xe::threading::Wait(pending_work_event_.get(), true);
      if (paused_) {
        pause_fence_.Signal();
        xe::threading::Wait(resume_event_.get(), false);
      }
      continue;
    }

    // Pace to kAudioIntervalSlack ahead of the deadline.
    const uint64_t wake_target_us = earliest_pump_us > kAudioIntervalSlack
                                        ? earliest_pump_us - kAudioIntervalSlack
                                        : 0;
    if (wake_target_us > now) {
      const std::chrono::milliseconds timeout((wake_target_us - now) / 1000);
      auto result =
          xe::threading::Wait(pending_work_event_.get(), true, timeout);
      if (result == xe::threading::WaitResult::kSuccess) {
        if (paused_) {
          pause_fence_.Signal();
          xe::threading::Wait(resume_event_.get(), false);
        }
        continue;
      }

      const uint64_t now_precise = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      if (wake_target_us > now_precise) {
        xe::threading::NanoSleepPrecise((wake_target_us - now_precise) * 1000);
      }
    }

    // Submit only if the host has a free output slot; otherwise drop (the host
    // queue is full, so it is already well buffered).
    if (xe::threading::Wait(client_semaphores_[client_index].get(), false,
                            std::chrono::milliseconds(0)) !=
        xe::threading::WaitResult::kSuccess) {
      continue;
    }

    RunClientCallback(client_index);
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

void AudioSystem::RunClientCallback(size_t client_index) {
  // UnregisterClient waits on this after clearing in_use.
  std::lock_guard<std::mutex> cb_lk(clients_[client_index].callback_mutex);
  uint32_t client_callback = 0;
  uint32_t client_callback_arg = 0;
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    if (clients_[client_index].in_use) {
      client_callback = clients_[client_index].callback;
      client_callback_arg = clients_[client_index].wrapped_callback_arg;
    }
  }
  if (client_callback) {
    SCOPE_profile_cpu_i("apu", "xe::apu::AudioSystem->client_callback");
    // XDroid360 AUD-2: audio underruns show producer gaps of 70-150 ms. Split
    // them into a guest callback that runs long (blocked in guest code) and a
    // worker that did not call it in time (no output slot freed). Only the
    // worker thread gets here. One line per second at most.
    thread_local uint64_t last_start_us = 0;
    thread_local uint64_t window_start_us = 0;
    thread_local uint64_t max_took_us = 0;
    thread_local uint64_t max_gap_us = 0;
    thread_local uint64_t total_took_us = 0;
    thread_local uint32_t calls = 0;
    // Callbacks slower than the 5.33 ms of audio they produce.
    thread_local uint32_t slow_calls = 0;
    const auto host_us = [] {
      return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count());
    };
#if XE_PLATFORM_ANDROID
    ApplyWorkerNice();
    thread_local uint64_t total_cpu_us = 0;
    thread_local uint64_t total_runq_us = 0;
    const CallbackSplit split_before = ReadCallbackSplit();
    // Forensic (AUD-2, temp): which guest waits the blocked time is spent in.
    thread_local kernel::XdroidWaitProbe wait_probe;
    kernel::xdroid_wait_probe = &wait_probe;
#endif
    const uint64_t start_us = host_us();
    if (last_start_us) {
      max_gap_us = std::max(max_gap_us, start_us - last_start_us);
    }
    last_start_us = start_us;
    uint64_t args[] = {client_callback_arg};
    processor_->Execute(worker_thread_->thread_state(), client_callback, args,
                        xe::countof(args));
    const uint64_t end_us = host_us();
    const uint64_t took_us = end_us - start_us;
#if XE_PLATFORM_ANDROID
    kernel::xdroid_wait_probe = nullptr;
    const CallbackSplit split_after = ReadCallbackSplit();
    total_cpu_us += (split_after.cpu_ns - split_before.cpu_ns) / 1000;
    total_runq_us += (split_after.runq_ns - split_before.runq_ns) / 1000;
#endif
    max_took_us = std::max(max_took_us, took_us);
    total_took_us += took_us;
    slow_calls += took_us > 5333;
    ++calls;
    // Every 10 s: the gap follows the device burst (40 ms in power_saving),
    // so it alone is no stall; the callback cost decides whether the pump
    // refills a burst in time.
    if (!window_start_us) {
      window_start_us = end_us;
    } else if (end_us - window_start_us >= 10000000) {
      XELOGI(
          "XDroid360: audio pump {} calls in {} ms: guest callback mean {} us, "
          "max {} ms, {} slower than 5.33 ms; gap between calls max {} ms",
          calls, (end_us - window_start_us) / 1000,
          calls ? total_took_us / calls : 0, max_took_us / 1000, slow_calls,
          max_gap_us / 1000);
#if XE_PLATFORM_ANDROID
      // Mean per call: on a core, runnable but waiting for one, and the rest
      // (blocked in a wait or sleeping).
      const uint64_t mean_us = calls ? total_took_us / calls : 0;
      const uint64_t cpu_us = calls ? total_cpu_us / calls : 0;
      const uint64_t runq_us = calls ? total_runq_us / calls : 0;
      XELOGI(
          "XDroid360: audio pump split per call: cpu {} us, run queue {} us, "
          "blocked {} us",
          cpu_us, runq_us,
          mean_us > cpu_us + runq_us ? mean_us - cpu_us - runq_us : 0);
      std::string waits;
      for (const auto& entry : wait_probe.entries) {
        if (entry.count) {
          waits += fmt::format(" [lr {:08X} kind {} n {} {} us]", entry.lr,
                               entry.kind, entry.count, entry.total_us);
        }
      }
      XELOGI("XDroid360: audio pump guest waits: {} us per call in waits{}",
             calls ? wait_probe.total_us / calls : 0, waits);
      wait_probe.Reset();
      auto& watch = kernel::xdroid_audio_watch;
      const uint64_t signals = watch.signals.exchange(0);
      const uint64_t latency = watch.signal_latency_us.exchange(0);
      XELOGI(
          "XDroid360: audio producer: {} signals, {} us from wait start to "
          "signal, signaler thread {:X} lr {:08X}; at wait start it was "
          "running {} ready {} blocked {} unknown {}; boosts {}",
          signals, signals ? latency / signals : 0, watch.signaler_tid.load(),
          watch.signaler_lr.load(), watch.producer_state[0].exchange(0),
          watch.producer_state[1].exchange(0),
          watch.producer_state[2].exchange(0),
          watch.producer_state[3].exchange(0), watch.boosts.exchange(0));
      const uint64_t dispatches = watch.dispatches.exchange(0);
      XELOGI(
          "XDroid360: audio producer ready-to-run delay: mean {} us, max {} "
          "us over {} wakes",
          dispatches ? watch.dispatch_delay_us.exchange(0) / dispatches : 0,
          watch.dispatch_delay_max_us.exchange(0), dispatches);
      // AUD-5: where the producer's render time goes on its host thread.
      // cpu ~ wall: it is CPU work (slow core or clock); runq large: its
      // dispatch thread waits for a core; neither: it is off its dispatch
      // thread between slices (guest ready queue or a wait).
      {
        const uint64_t slices = watch.slices.exchange(0);
        const uint64_t per = signals ? signals : 1;
        std::string cores;
        for (int i = 0; i < kernel::XdroidAudioWatch::kHostCpus; ++i) {
          const uint64_t us = watch.host_cpu_us[i].exchange(0);
          if (us) {
            cores += fmt::format(" {}:{}", i, us / per);
          }
        }
        static bool capacities_logged = false;
        if (!capacities_logged && slices) {
          capacities_logged = true;
          std::string capacities;
          for (int i = 0; i < kernel::XdroidAudioWatch::kHostCpus; ++i) {
            char path[64];
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
            if (FILE* file = std::fopen(path, "r")) {
              int capacity = 0;
              if (std::fscanf(file, "%d", &capacity) == 1) {
                capacities += fmt::format(" {}:{}", i, capacity);
              }
              std::fclose(file);
            }
          }
          XELOGI("XDroid360: host core capacities:{}", capacities);
        }
        XELOGI(
            "XDroid360: audio producer host split per render: {} slices, wall "
            "{} us, cpu {} us, host runq {} us; slice ends wait {} preempt {} "
            "quantum {} yield {}; cpu us per render by host core:{}; dispatch "
            "nice {} (boost {})",
            slices, watch.slice_wall_us.exchange(0) / per,
            watch.slice_cpu_us.exchange(0) / per,
            watch.slice_runq_us.exchange(0) / per,
            watch.slice_end_wait.exchange(0),
            watch.slice_end_preempt.exchange(0),
            watch.slice_end_quantum.exchange(0),
            watch.slice_end_yield.exchange(0), cores,
            watch.dispatch_nice.load(), cvars::xdroid_audio_producer_nice);
      }
      total_cpu_us = 0;
      total_runq_us = 0;
#endif
      window_start_us = end_us;
      max_took_us = 0;
      max_gap_us = 0;
      total_took_us = 0;
      calls = 0;
      slow_calls = 0;
    }
  }
}

void AudioSystem::ConsumerPacedWorkerLoop() {
  std::vector<xe::threading::WaitHandle*> handles;
  std::vector<size_t> handle_clients;
  while (worker_running_) {
    handles.clear();
    handle_clients.clear();
    handles.push_back(pending_work_event_.get());
    handle_clients.push_back(kMaximumClientCount);
    {
      std::lock_guard<std::mutex> clients_lock(clients_mutex_);
      for (size_t i = 0; i < kMaximumClientCount; ++i) {
        if (clients_[i].in_use) {
          handles.push_back(client_semaphores_[i].get());
          handle_clients.push_back(i);
        }
      }
    }
    // A slot frees up every ~5.3 ms while the output runs. Registering or
    // unregistering a client sets pending_work_event_, so the list is rebuilt
    // without a timeout and the thread sleeps while no client exists.
    auto [result, index] =
        xe::threading::WaitAny(handles.data(), handles.size(), true);
    if (result != xe::threading::WaitResult::kSuccess) {
      continue;
    }
    if (index == 0) {
      // Registration change, pause or shutdown.
      if (paused_) {
        pause_fence_.Signal();
        xe::threading::Wait(resume_event_.get(), false);
      }
      continue;
    }
    RunClientCallback(handle_clients[index]);
  }
}

int AudioSystem::FindFreeClient() {
  for (int i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  worker_running_ = false;
  pending_work_event_->Set();
  if (worker_thread_) {
    worker_thread_->Wait(0, 0, 0, nullptr);
    worker_thread_.reset();
  }

  // Unregister all active clients to shut down their audio drivers before
  // the semaphores are destroyed with this AudioSystem.
  uint32_t wrapped_callback_args[kMaximumClientCount] = {};
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    for (size_t i = 0; i < kMaximumClientCount; ++i) {
      if (clients_[i].in_use) {
        DestroyDriver(clients_[i].driver);
        wrapped_callback_args[i] = clients_[i].wrapped_callback_arg;
        clients_[i].driver = nullptr;
        clients_[i].callback = 0;
        clients_[i].callback_arg = 0;
        clients_[i].wrapped_callback_arg = 0;
        clients_[i].in_use = false;
      }
    }
  }
  // Outside clients_mutex_, as the heap takes the global lock.
  for (uint32_t wrapped_callback_arg : wrapped_callback_args) {
    if (wrapped_callback_arg) {
      memory()->SystemHeapFree(wrapped_callback_arg);
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg,
                                     size_t* out_index) {
  // Outside clients_mutex_, as the heap takes the global lock.
  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  xe::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  std::unique_lock<std::mutex> clients_lock(clients_mutex_);

  auto index = FindFreeClient();
  assert_true(index >= 0);
  if (index < 0) {
    clients_lock.unlock();
    memory()->SystemHeapFree(ptr);
    return X_STATUS_UNSUCCESSFUL;
  }

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    XELOGE("AudioSystem::RegisterClient: CreateDriver failed for index={}",
           index);
    clients_lock.unlock();
    memory()->SystemHeapFree(ptr);
    return result;
  }
  assert_not_null(driver);
  XELOGI(
      "AudioSystem::RegisterClient: driver created for index={}, driver={:p}",
      index, (void*)driver);

  clients_[index].driver = driver;
  clients_[index].callback = callback;
  clients_[index].callback_arg = callback_arg;
  clients_[index].wrapped_callback_arg = ptr;
  clients_[index].in_use = true;
  clients_[index].next_pump_us = 0;
  clients_[index].frames_submitted.store(0);
  clients_[index].frames_processed.store(0);
  clients_[index].frames_dropped.store(0);

  // Wake the worker so it re-scans and starts pacing this client immediately.
  pending_work_event_->Set();

  XELOGI("AudioSystem::RegisterClient: client {} registered successfully",
         index);

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, float* samples) {
  SCOPE_profile_cpu_f("apu");

  std::lock_guard<std::mutex> clients_lock(clients_mutex_);
  assert_true(index < kMaximumClientCount);
  if (index >= kMaximumClientCount || !clients_[index].in_use ||
      !clients_[index].driver) {
    XELOGW(
        "SubmitFrame called for invalid/unregistered client index {} "
        "(in_use={}, driver={:p})",
        index, index < kMaximumClientCount ? clients_[index].in_use : false,
        index < kMaximumClientCount ? (void*)clients_[index].driver : nullptr);

    // Submit silence instead of dropping the frame to maintain the callback
    // chain.  If we don't submit anything, the audio driver's OnBufferEnd
    // callback will never fire, causing the semaphore to leak.
    if (index < kMaximumClientCount && clients_[index].driver) {
      static float silence[apu::AudioDriver::kFrameSamplesMax] = {0};
      clients_[index].frames_dropped++;
      (clients_[index].driver)->SubmitFrame(silence);
    } else if (index < kMaximumClientCount) {
      // Tick the semaphore so the worker doesn't stall on a dead client.
      client_semaphores_[index]->Release(1, nullptr);
    }
    return;
  }
  clients_[index].frames_submitted++;
  clients_[index].frames_processed++;
  (clients_[index].driver)->SubmitFrame(samples);
}

bool AudioSystem::GetClientPerformance(size_t index,
                                       ClientPerformance* out_perf) {
  if (index >= kMaximumClientCount || !out_perf) {
    return false;
  }

  if (!clients_[index].in_use) {
    return false;
  }

  out_perf->frames_submitted = clients_[index].frames_submitted.load();
  out_perf->frames_processed = clients_[index].frames_processed.load();
  out_perf->frames_dropped = clients_[index].frames_dropped.load();
  return true;
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  assert_true(index < kMaximumClientCount);
  AudioDriver* driver_to_destroy;
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    XELOGI(
        "AudioSystem::UnregisterClient: index={}, driver={:p}", index,
        index < kMaximumClientCount ? (void*)clients_[index].driver : nullptr);
    driver_to_destroy = clients_[index].driver;
    // Leak wrapped_callback_arg: in-flight callback may hold this pointer.
    clients_[index].driver = nullptr;
    clients_[index].callback = 0;
    clients_[index].callback_arg = 0;
    clients_[index].wrapped_callback_arg = 0;
    clients_[index].in_use = false;
    clients_[index].next_pump_us = 0;
    clients_[index].frames_submitted.store(0);
    clients_[index].frames_processed.store(0);
    clients_[index].frames_dropped.store(0);
  }
  // Drop the client's semaphore from the worker's wait list.
  pending_work_event_->Set();

  // Wait for any in-flight callback; can't hold clients_mutex_ (callback
  // re-enters).
  {
    std::lock_guard<std::mutex> lk(clients_[index].callback_mutex);
  }

  DestroyDriver(driver_to_destroy);

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  xe::threading::WaitResult wait_result;
  do {
    wait_result = xe::threading::Wait(client_semaphore, false,
                                      std::chrono::milliseconds(0));
  } while (wait_result == xe::threading::WaitResult::kSuccess);
  assert_true(wait_result == xe::threading::WaitResult::kTimeout);
}

bool AudioSystem::Save(ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (int i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    XELOGE("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    assert_true(id < kMaximumClientCount);

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.next_pump_us = 0;
    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(queued_frames_, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      XELOGE(
          "AudioSystem::Restore - Call to CreateDriver failed with status "
          "{:08X}",
          status);
      return false;
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  pending_work_event_->Set();
  pause_fence_.Wait();

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace apu
}  // namespace xe
