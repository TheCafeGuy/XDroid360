/**
 * XDroid360: per-frame ADPF performance hint and thermal tier (ADPF-1).
 *
 * The app's PerfHint.kt reports once per second the busiest thread's CPU time
 * divided by the frames presented. At the 30 FPS cap the guest main thread
 * spends most of its frame spinning on a GPU fence (db16cyc sled, CE-5/CE-7),
 * and that spin is CPU time: the hint then reads "the frame took the whole
 * period" whatever the clock, and the power HAL never lowers it. This module
 * reports the same quantity per frame from native code, minus the spin.
 *
 * xdroid_adpf (live):
 *   0 = off: the app's 1 s hint runs as before (default).
 *   1 = native per-frame hint; work = busiest session thread's CPU time minus
 *       its guest spin time (spin is only measured when the db16cyc helper is
 *       emitted: guest_spin_park 1 or 2; with 0 it equals arm 2).
 *   2 = native per-frame hint; work = raw CPU time (spin counted, control).
 *   3 = observe only: no session, same numbers logged (no-hint control).
 * The app's hint steps aside whenever live.toml sets xdroid_adpf != 0.
 *
 * Session threads: those whose name starts with an entry of
 * xdroid_adpf_threads (guest dispatch threads + the command processor). Each
 * frame (one futex wake per guest swap) every session thread's CPU clock is
 * read; the busiest one is the frame's critical path. Target = the frame
 * period the game runs at (30/40/60 tier from measured swaps) times
 * xdroid_adpf_target_pct.
 *
 * Audio (xdroid_adpf_audio = 1, live, needs xdroid_adpf != 0; with 3 it is the
 * only session): a second session for the host audio threads
 * (xdroid_adpf_audio_threads) plus the "Guest CPU N" dispatch thread that runs
 * the guest audio producer (guest thread id xdroid_adpf_audio_guest_tid, AC6:
 * the XAudio2 mixer), re-resolved every 2 s. The frame session lowers clocks
 * when the frame has slack, which can strand that thread on a little core
 * while the mixer's blocks grow (0.8 -> 4.4 ms hot). Its own loop reports the
 * busiest audio thread's utilization (CPU minus spin over the wake interval)
 * times the block period (xdroid_adpf_audio_period_us), against
 * xdroid_adpf_audio_target_pct of that period: above it the HAL boosts the
 * thread. A dispatch thread hosts other fibers too, so this over-reports
 * rather than under-reports.
 *
 * Thermal: every 2 s the 10 s headroom forecast picks a frame cap tier
 * (60/40/30; step down at once, step up after 30 s), logged and exported for
 * the adaptive frame target (P60-3). Nothing acts on it yet.
 *
 * Log: one XDROID-ADPF line every 10 s; tools pick arms apart by `mode=`.
 */

#include "xdroid_adpf.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/spin_accounting.h"
#include "xenia/base/spin_sites.h"
#include "xenia/base/wake_generation.h"
#include "xenia/emulator.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

DEFINE_int32(xdroid_adpf, 0,
             "XDroid360 (ADPF-1): native per-frame performance hint. 0 = off "
             "(app 1 s hint), 1 = hint without guest spin, 2 = hint with raw "
             "CPU time, 3 = observe and log only. Live.",
             "XDroid360");
DEFINE_int32(xdroid_adpf_target_pct, 85,
             "Hint target work duration in percent of the frame period. Live.",
             "XDroid360");
DEFINE_bool(xdroid_adpf_power_efficiency, false,
            "Ask the hint session to prefer power efficiency over boosting "
            "(Android 15+). Live.",
            "XDroid360");
DEFINE_int32(xdroid_adpf_audio, 0,
             "XDroid360 (ADPF-1): 1 = separate hint session for the audio "
             "threads (needs xdroid_adpf != 0). Live.",
             "XDroid360");
DEFINE_int32(xdroid_adpf_audio_guest_tid, 0,
             "Guest thread id of the audio producer (AC6 XAudio2 mixer: 18); "
             "the dispatch thread running it joins the audio session. 0 = "
             "none. Live.",
             "XDroid360");
DEFINE_int32(xdroid_adpf_audio_period_us, 5333,
             "Audio block period for the audio session (256 samples at "
             "48 kHz). Live.",
             "XDroid360");
DEFINE_int32(xdroid_adpf_audio_target_pct, 50,
             "Audio session target: busiest audio thread's busy percent "
             "above which the HAL is asked to boost it. Live.",
             "XDroid360");
DEFINE_string(xdroid_adpf_audio_threads, "Audio Worker,XMA Decoder",
              "Comma-separated host thread name prefixes put in the audio "
              "session.",
              "XDroid360");
DEFINE_string(xdroid_adpf_threads, "Guest CPU,GPU Commands",
              "Comma-separated thread name prefixes put in the hint session.",
              "XDroid360");

namespace xdroid {
namespace {

// NDK entry points, resolved at run time: minSdk is 29 and these need API
// 31-35. Opaque types are local so no NDK header version is required.
struct HintManager;
struct HintSession;
struct ThermalManager;
struct ThermalThreshold {
  float headroom;
  int thermal_status;
};

struct Ndk {
  HintManager* (*get_manager)() = nullptr;
  HintSession* (*create_session)(HintManager*, const int32_t*, size_t,
                                 int64_t) = nullptr;
  int (*update_target)(HintSession*, int64_t) = nullptr;
  int (*report_actual)(HintSession*, int64_t) = nullptr;
  void (*close_session)(HintSession*) = nullptr;
  int (*set_threads)(HintSession*, const pid_t*, size_t) = nullptr;
  int (*set_prefer_power_efficiency)(HintSession*, bool) = nullptr;
  ThermalManager* (*acquire_thermal)() = nullptr;
  int (*thermal_status)(ThermalManager*) = nullptr;
  float (*thermal_headroom)(ThermalManager*, int) = nullptr;
  int (*thermal_thresholds)(ThermalManager*, const ThermalThreshold**,
                            size_t*) = nullptr;

  void Load() {
    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      return;
    }
    Resolve(lib, "APerformanceHint_getManager", get_manager);
    Resolve(lib, "APerformanceHint_createSession", create_session);
    Resolve(lib, "APerformanceHint_updateTargetWorkDuration", update_target);
    Resolve(lib, "APerformanceHint_reportActualWorkDuration", report_actual);
    Resolve(lib, "APerformanceHint_closeSession", close_session);
    Resolve(lib, "APerformanceHint_setThreads", set_threads);
    Resolve(lib, "APerformanceHint_setPreferPowerEfficiency",
            set_prefer_power_efficiency);
    Resolve(lib, "AThermal_acquireManager", acquire_thermal);
    Resolve(lib, "AThermal_getCurrentThermalStatus", thermal_status);
    Resolve(lib, "AThermal_getThermalHeadroom", thermal_headroom);
    Resolve(lib, "AThermal_getThermalHeadroomThresholds", thermal_thresholds);
  }

  bool HasHint() const {
    return get_manager && create_session && update_target && report_actual &&
           close_session;
  }

 private:
  template <typename Fn>
  static void Resolve(void* lib, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(dlsym(lib, name));
  }
};

std::atomic<int> recommended_cap{0};

std::mutex emulator_mutex;
xe::Emulator* emulator_ptr = nullptr;

// "Guest CPU N" index running guest thread `guest_tid`, or -1.
int GuestThreadDispatchCpu(uint32_t guest_tid) {
  std::lock_guard<std::mutex> lock(emulator_mutex);
  if (!emulator_ptr || !guest_tid) {
    return -1;
  }
  xe::kernel::KernelState* kernel = emulator_ptr->kernel_state();
  if (!kernel || !kernel->guest_scheduler()) {
    return -1;
  }
  auto thread = kernel->GetThreadByID(guest_tid);
  if (!thread) {
    return -1;
  }
  return kernel->guest_scheduler()->DispatchCpuOf(thread->active_cpu());
}

constexpr int64_t kNsPerMs = 1000000;
constexpr int64_t kFramePeriodNs[] = {16666667, 25000000, 33333333};
constexpr int kTierFps[] = {60, 40, 30};

// CPU time of another thread of this process (Linux per-thread CPU clock id:
// MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)). False once the thread is gone.
bool ThreadCpuNs(int32_t tid, uint64_t& out) {
  const clockid_t clock = clockid_t((~unsigned(tid) << 3) | 6);
  timespec ts;
  if (clock_gettime(clock, &ts) != 0) {
    return false;
  }
  out = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
  return true;
}

uint64_t SelfCpuNs() {
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

struct Tracked {
  int32_t tid;
  std::string name;
  uint64_t cpu_base;
  uint64_t spin_base;
  uint32_t critical_frames;
};

std::vector<std::string> SplitPrefixes(const std::string& list) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) {
      end = list.size();
    }
    if (end > start) {
      out.emplace_back(list.substr(start, end - start));
    }
    start = end + 1;
  }
  return out;
}

// Threads of this process whose name starts with one of `prefixes`, by tid.
std::vector<Tracked> ScanThreads(const std::vector<std::string>& prefixes,
                                 const std::string& exact = std::string()) {
  std::vector<Tracked> out;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) {
    return out;
  }
  while (dirent* entry = readdir(dir)) {
    const int32_t tid = int32_t(std::atoi(entry->d_name));
    if (tid <= 0) {
      continue;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    FILE* file = std::fopen(path, "re");
    if (!file) {
      continue;
    }
    char comm[32] = {};
    const bool read = std::fgets(comm, sizeof(comm), file) != nullptr;
    std::fclose(file);
    if (!read) {
      continue;
    }
    comm[std::strcspn(comm, "\n")] = '\0';
    if (!exact.empty() && exact == comm) {
      out.push_back({tid, comm, 0, 0, 0});
      continue;
    }
    for (const std::string& prefix : prefixes) {
      if (std::strncmp(comm, prefix.c_str(), prefix.size()) == 0) {
        out.push_back({tid, comm, 0, 0, 0});
        break;
      }
    }
  }
  closedir(dir);
  std::sort(out.begin(), out.end(),
            [](const Tracked& a, const Tracked& b) { return a.tid < b.tid; });
  return out;
}

double Percentile(std::vector<double>& values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  const size_t index =
      std::min(values.size() - 1, size_t(fraction * double(values.size())));
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
}

class Adpf {
 public:
  void Run() {
    pthread_setname_np(pthread_self(), "XDroid ADPF");
    ndk_.Load();
    XELOGI(
        "XDROID-ADPF: hint api {} set_threads {} power_efficiency {} thermal "
        "headroom {}",
        ndk_.HasHint(), ndk_.set_threads != nullptr,
        ndk_.set_prefer_power_efficiency != nullptr,
        ndk_.thermal_headroom != nullptr);
    if (ndk_.acquire_thermal) {
      thermal_ = ndk_.acquire_thermal();
      LogThermalThresholds();
    }
    xe::threading::WakeGeneration& frames_generation =
        xe::threading::PresentFrameGeneration();
    while (true) {
      LogSpinSites(std::chrono::steady_clock::now());
      const int mode = cvars::xdroid_adpf;
      if (mode < 1 || mode > 3) {
        if (mode_ != 0) {
          Stop();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
      if (mode != mode_) {
        Start(mode);
      }
      frames_generation.WaitFor(seen_generation_,
                                std::chrono::milliseconds(250));
      seen_generation_ = frames_generation.Load();
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_housekeeping_) {
        Housekeeping(now);
      }
      OnWake(now);
      if (now >= next_log_) {
        Log(now);
      }
    }
  }

 private:
  // CE-7b: the busiest guest spin sites (cumulative), for the wait-engine
  // study (XR-3) and to check what guest_spin_park=3 learned. Filled only
  // while guest_spin_park != 0.
  void LogSpinSites(std::chrono::steady_clock::time_point now) {
    if (now < next_spin_log_) {
      return;
    }
    next_spin_log_ = now + std::chrono::seconds(10);
    struct Row {
      uint32_t pc, spins, released, parks, recent;
      uint64_t ms;
    };
    std::vector<Row> rows;
    const xe::threading::SpinSite* sites = xe::threading::SpinSites();
    for (size_t i = 0; i < xe::threading::kMaxSpinSites; ++i) {
      const uint32_t pc = sites[i].pc.load(std::memory_order_acquire);
      if (!pc) {
        continue;
      }
      rows.push_back({pc, sites[i].spins.load(std::memory_order_relaxed),
                      sites[i].gpu_released.load(std::memory_order_relaxed),
                      sites[i].parks.load(std::memory_order_relaxed),
                      sites[i].recent_gpu.load(std::memory_order_relaxed) *
                          100 / 65536,
                      sites[i].spin_ns.load(std::memory_order_relaxed) /
                          1000000});
    }
    if (rows.empty()) {
      return;
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.ms > b.ms; });
    std::string line;
    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
      const Row& row = rows[i];
      line += fmt::format(" {:08X}:n={},gpu={}%,recent={}%,park={},ms={}",
                          row.pc, row.spins,
                          row.spins ? 100ull * row.released / row.spins : 0,
                          row.recent, row.parks, row.ms);
    }
    XELOGI("XDROID-SPIN sites={}{}", rows.size(), line);
  }

  void Start(int mode) {
    XELOGI("XDROID-ADPF: mode {} -> {}", mode_, mode);
    mode_ = mode;
    xe::threading::SpinAccountingEnabled().store(true,
                                                 std::memory_order_relaxed);
    if (mode_ == 3) {
      CloseSession();
    }
    seen_generation_ = xe::threading::PresentFrameGeneration().Load();
    last_frame_ = xe::logging::GetFrameNumber();
    last_frame_time_ = std::chrono::steady_clock::now();
    next_housekeeping_ = last_frame_time_;
    next_scan_ = last_frame_time_;
    next_log_ = last_frame_time_ + std::chrono::seconds(10);
    log_start_ = last_frame_time_;
    self_cpu_base_ = SelfCpuNs();
    ResetWindow();
  }

  void Stop() {
    XELOGI("XDROID-ADPF: mode {} -> 0", mode_);
    CloseSession();
    CloseAudioSession();
    audio_threads_.clear();
    audio_dispatch_ = -2;
    xe::threading::SpinAccountingEnabled().store(false,
                                                 std::memory_order_relaxed);
    threads_.clear();
    mode_ = 0;
    recommended_cap.store(0, std::memory_order_relaxed);
  }

  void Housekeeping(std::chrono::steady_clock::time_point now) {
    next_housekeeping_ = now + std::chrono::seconds(2);
    SampleThermal(now);
    UpdateAudioThreads(now);
    if (now < next_scan_) {
      return;
    }
    // Guest threads are created at title start; a slow rescan catches them.
    next_scan_ = now + std::chrono::seconds(5);
    std::vector<Tracked> scanned =
        ScanThreads(SplitPrefixes(cvars::xdroid_adpf_threads));
    bool same = scanned.size() == threads_.size();
    for (size_t i = 0; same && i < scanned.size(); ++i) {
      same = scanned[i].tid == threads_[i].tid;
    }
    if (same) {
      return;
    }
    threads_ = std::move(scanned);
    Rebaseline();
    std::string names;
    for (const Tracked& thread : threads_) {
      names += thread.name + "/" + std::to_string(thread.tid) + " ";
    }
    XELOGI("XDROID-ADPF: threads {}", names);
    if (session_ && ndk_.set_threads) {
      std::vector<pid_t> tids;
      for (const Tracked& thread : threads_) {
        tids.push_back(pid_t(thread.tid));
      }
      if (ndk_.set_threads(session_, tids.data(), tids.size()) == 0) {
        return;
      }
    }
    CloseSession();
  }

  void OnWake(std::chrono::steady_clock::time_point now) {
    AudioOnWake(now);
    const uint32_t frame = xe::logging::GetFrameNumber();
    const uint32_t frames = frame - last_frame_;
    if (!frames) {
      // Loading or paused: what ran meanwhile is no frame's work.
      Rebaseline();
      last_frame_time_ = now;
      return;
    }
    const int64_t interval_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - last_frame_time_)
            .count();
    last_frame_ = frame;
    last_frame_time_ = now;
    UpdateTier(interval_ns / int64_t(frames));

    uint64_t raw_max = 0, net_max = 0;
    Tracked* critical = nullptr;
    for (Tracked& thread : threads_) {
      uint64_t cpu;
      if (!ThreadCpuNs(thread.tid, cpu)) {
        // Thread gone: rescan on the next housekeeping pass.
        next_scan_ = now;
        continue;
      }
      const uint64_t spin =
          xe::threading::SpinNanosecondsForThread(thread.tid);
      const uint64_t raw = cpu - thread.cpu_base;
      const uint64_t spun = std::min(raw, spin - thread.spin_base);
      thread.cpu_base = cpu;
      thread.spin_base = spin;
      window_cpu_ns_ += raw;
      window_spin_ns_ += spun;
      raw_max = std::max(raw_max, raw);
      if (raw - spun >= net_max) {
        net_max = raw - spun;
        critical = &thread;
      }
    }
    if (critical) {
      ++critical->critical_frames;
    }
    const double raw_ms = double(raw_max) / frames / kNsPerMs;
    const double net_ms = double(net_max) / frames / kNsPerMs;
    window_raw_ms_.push_back(raw_ms);
    window_net_ms_.push_back(net_ms);
    window_frames_ += frames;

    if (mode_ == 3 || threads_.empty() || !ndk_.HasHint()) {
      return;
    }
    const int64_t target_ns =
        kFramePeriodNs[tier_] *
        std::clamp(int64_t(cvars::xdroid_adpf_target_pct), int64_t(25),
                   int64_t(150)) /
        100;
    if (!session_ && !OpenSession(target_ns)) {
      return;
    }
    if (target_ns != target_ns_) {
      if (ndk_.update_target(session_, target_ns) == 0) {
        target_ns_ = target_ns;
      } else {
        ++window_errors_;
      }
    }
    const bool power_efficiency = cvars::xdroid_adpf_power_efficiency;
    if (power_efficiency != power_efficiency_ &&
        ndk_.set_prefer_power_efficiency) {
      ndk_.set_prefer_power_efficiency(session_, power_efficiency);
      power_efficiency_ = power_efficiency;
    }
    const uint64_t work_ns = (mode_ == 1 ? net_max : raw_max) / frames;
    const int64_t report_ns =
        std::clamp(int64_t(work_ns), int64_t(500000), int64_t(200 * kNsPerMs));
    if (ndk_.report_actual(session_, report_ns) == 0) {
      ++window_reports_;
    } else {
      // EPIPE and the like: the session died; reopen on the next frame.
      ++window_errors_;
      CloseSession();
    }
  }

  // The frame period tier the game runs at: switch only after 30 frames in a
  // row snap to another tier, so one long frame never moves the target.
  void UpdateTier(int64_t frame_ns) {
    int snapped = 2;
    if (frame_ns < 20 * kNsPerMs) {
      snapped = 0;
    } else if (frame_ns < 29 * kNsPerMs) {
      snapped = 1;
    }
    if (snapped == tier_) {
      tier_streak_ = 0;
      return;
    }
    if (snapped != tier_candidate_) {
      tier_candidate_ = snapped;
      tier_streak_ = 0;
    }
    if (++tier_streak_ >= 30) {
      tier_ = snapped;
      tier_streak_ = 0;
    }
  }

  void UpdateAudioThreads(std::chrono::steady_clock::time_point now) {
    if (cvars::xdroid_adpf_audio != 1) {
      if (audio_session_ || !audio_threads_.empty()) {
        CloseAudioSession();
        audio_threads_.clear();
        audio_dispatch_ = -2;
      }
      return;
    }
    const int dispatch =
        GuestThreadDispatchCpu(uint32_t(cvars::xdroid_adpf_audio_guest_tid));
    if (dispatch == audio_dispatch_ && now < next_audio_scan_) {
      return;
    }
    next_audio_scan_ = now + std::chrono::seconds(5);
    std::vector<Tracked> scanned =
        ScanThreads(SplitPrefixes(cvars::xdroid_adpf_audio_threads),
                    dispatch >= 0
                        ? "Guest CPU " + std::to_string(dispatch)
                        : std::string());
    const bool dispatch_changed = dispatch != audio_dispatch_;
    audio_dispatch_ = dispatch;
    bool same = scanned.size() == audio_threads_.size();
    for (size_t i = 0; same && i < scanned.size(); ++i) {
      same = scanned[i].tid == audio_threads_[i].tid;
    }
    if (same) {
      return;
    }
    audio_threads_ = std::move(scanned);
    for (Tracked& thread : audio_threads_) {
      ThreadCpuNs(thread.tid, thread.cpu_base);
      thread.spin_base = xe::threading::SpinNanosecondsForThread(thread.tid);
    }
    audio_last_ = now;
    std::string names;
    for (const Tracked& thread : audio_threads_) {
      names += thread.name + "/" + std::to_string(thread.tid) + " ";
    }
    XELOGI("XDROID-ADPF: audio threads {}(producer dispatch cpu {}{})", names,
           dispatch, dispatch_changed ? ", moved" : "");
    if (audio_session_ && ndk_.set_threads) {
      std::vector<pid_t> tids;
      for (const Tracked& thread : audio_threads_) {
        tids.push_back(pid_t(thread.tid));
      }
      if (ndk_.set_threads(audio_session_, tids.data(), tids.size()) == 0) {
        return;
      }
    }
    CloseAudioSession();
  }

  void AudioOnWake(std::chrono::steady_clock::time_point now) {
    if (cvars::xdroid_adpf_audio != 1 || audio_threads_.empty()) {
      return;
    }
    const int64_t elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - audio_last_)
            .count();
    if (elapsed_ns < 5 * kNsPerMs) {
      return;
    }
    audio_last_ = now;
    uint64_t busiest_ns = 0;
    for (Tracked& thread : audio_threads_) {
      uint64_t cpu;
      if (!ThreadCpuNs(thread.tid, cpu)) {
        next_audio_scan_ = now;
        continue;
      }
      const uint64_t spin =
          xe::threading::SpinNanosecondsForThread(thread.tid);
      const uint64_t raw = cpu - thread.cpu_base;
      const uint64_t net = raw - std::min(raw, spin - thread.spin_base);
      thread.cpu_base = cpu;
      thread.spin_base = spin;
      busiest_ns = std::max(busiest_ns, net);
    }
    const double utilization =
        std::min(1.0, double(busiest_ns) / double(elapsed_ns));
    window_audio_util_.push_back(100.0 * utilization);
    if (!ndk_.HasHint()) {
      return;
    }
    const int64_t period_ns =
        std::clamp(int64_t(cvars::xdroid_adpf_audio_period_us), int64_t(1000),
                   int64_t(50000)) *
        1000;
    const int64_t target_ns =
        period_ns *
        std::clamp(int64_t(cvars::xdroid_adpf_audio_target_pct), int64_t(10),
                   int64_t(100)) /
        100;
    if (!audio_session_) {
      std::vector<int32_t> tids;
      for (const Tracked& thread : audio_threads_) {
        tids.push_back(thread.tid);
      }
      audio_session_ = CreateSession(tids, target_ns);
      if (!audio_session_) {
        ++window_audio_errors_;
        return;
      }
      audio_target_ns_ = target_ns;
      XELOGI("XDROID-ADPF: audio session open, {} threads, target {:.2f} ms",
             tids.size(), double(target_ns) / kNsPerMs);
    }
    if (target_ns != audio_target_ns_) {
      if (ndk_.update_target(audio_session_, target_ns) == 0) {
        audio_target_ns_ = target_ns;
      } else {
        ++window_audio_errors_;
      }
    }
    const int64_t report_ns = std::max(
        int64_t(100000), int64_t(utilization * double(period_ns)));
    if (ndk_.report_actual(audio_session_, report_ns) == 0) {
      ++window_audio_reports_;
    } else {
      ++window_audio_errors_;
      CloseAudioSession();
    }
  }

  HintSession* CreateSession(const std::vector<int32_t>& tids,
                             int64_t target_ns) {
    if (tids.empty()) {
      return nullptr;
    }
    if (!manager_) {
      manager_ = ndk_.get_manager();
      if (!manager_) {
        return nullptr;
      }
    }
    return ndk_.create_session(manager_, tids.data(), tids.size(), target_ns);
  }

  void CloseAudioSession() {
    if (audio_session_) {
      ndk_.close_session(audio_session_);
      audio_session_ = nullptr;
    }
  }

  bool OpenSession(int64_t target_ns) {
    std::vector<int32_t> tids;
    for (const Tracked& thread : threads_) {
      tids.push_back(thread.tid);
    }
    session_ = CreateSession(tids, target_ns);
    if (!session_) {
      ++window_errors_;
      return false;
    }
    target_ns_ = target_ns;
    power_efficiency_ = false;
    XELOGI("XDROID-ADPF: session open, {} threads, target {:.1f} ms",
           tids.size(), double(target_ns) / kNsPerMs);
    return true;
  }

  void CloseSession() {
    if (session_) {
      ndk_.close_session(session_);
      session_ = nullptr;
    }
  }

  void Rebaseline() {
    for (Tracked& thread : threads_) {
      ThreadCpuNs(thread.tid, thread.cpu_base);
      thread.spin_base = xe::threading::SpinNanosecondsForThread(thread.tid);
    }
  }

  void SampleThermal(std::chrono::steady_clock::time_point now) {
    if (!thermal_) {
      return;
    }
    if (ndk_.thermal_status) {
      thermal_status_ = ndk_.thermal_status(thermal_);
    }
    if (!ndk_.thermal_headroom) {
      return;
    }
    // At most one call per second, or the API returns NaN.
    const float headroom = ndk_.thermal_headroom(thermal_, 10);
    if (std::isnan(headroom)) {
      return;
    }
    headroom_ = headroom;
    // 1.0 = SEVERE expected in 10 s. Step down at once, up after 30 s.
    int cap = 60;
    if (headroom >= 0.95f) {
      cap = 30;
    } else if (headroom >= 0.85f) {
      cap = 40;
    }
    const int current = recommended_cap.load(std::memory_order_relaxed);
    if (!current || cap < current) {
      recommended_cap.store(cap, std::memory_order_relaxed);
      raise_since_ = now;
      if (current) {
        XELOGI("XDROID-ADPF: thermal cap {} -> {} (headroom {:.2f})",
               current, cap, headroom);
      }
      return;
    }
    if (cap == current) {
      raise_since_ = now;
      return;
    }
    // Raising needs headroom below the step-down line by 0.05 for 30 s.
    const float raise_line = current == 30 ? 0.80f : 0.70f;
    if (headroom >= raise_line) {
      raise_since_ = now;
    } else if (now - raise_since_ >= std::chrono::seconds(30)) {
      const int raised = current == 30 ? 40 : 60;
      recommended_cap.store(raised, std::memory_order_relaxed);
      raise_since_ = now;
      XELOGI("XDROID-ADPF: thermal cap {} -> {} (headroom {:.2f})", current,
             raised, headroom);
    }
  }

  void LogThermalThresholds() {
    if (!thermal_ || !ndk_.thermal_thresholds) {
      return;
    }
    const ThermalThreshold* thresholds = nullptr;
    size_t count = 0;
    if (ndk_.thermal_thresholds(thermal_, &thresholds, &count) != 0 ||
        !thresholds) {
      return;
    }
    std::string text;
    for (size_t i = 0; i < count; ++i) {
      char item[48];
      std::snprintf(item, sizeof(item), "%d:%.2f ",
                    thresholds[i].thermal_status, thresholds[i].headroom);
      text += item;
    }
    std::free(const_cast<ThermalThreshold*>(thresholds));
    XELOGI("XDROID-ADPF: thermal headroom thresholds (status:headroom) {}",
           text);
  }

  void Log(std::chrono::steady_clock::time_point now) {
    next_log_ = now + std::chrono::seconds(10);
    const double seconds =
        std::chrono::duration<double>(now - log_start_).count();
    log_start_ = now;
    const uint64_t self_cpu = SelfCpuNs();
    const double self_ms = double(self_cpu - self_cpu_base_) / kNsPerMs;
    self_cpu_base_ = self_cpu;
    const Tracked* busiest = nullptr;
    for (const Tracked& thread : threads_) {
      if (!busiest || thread.critical_frames > busiest->critical_frames) {
        busiest = &thread;
      }
    }
    const double net_p50 = Percentile(window_net_ms_, 0.5);
    const double net_p90 = Percentile(window_net_ms_, 0.9);
    const double net_max =
        window_net_ms_.empty()
            ? 0.0
            : *std::max_element(window_net_ms_.begin(), window_net_ms_.end());
    const double raw_p50 = Percentile(window_raw_ms_, 0.5);
    const double audio_p50 = Percentile(window_audio_util_, 0.5);
    const double audio_max =
        window_audio_util_.empty()
            ? 0.0
            : *std::max_element(window_audio_util_.begin(),
                                window_audio_util_.end());
    XELOGI(
        "XDROID-ADPF mode={} fps={:.1f} tier={} target={:.1f}ms net_p50={:.1f} "
        "net_p90={:.1f} net_max={:.1f} raw_p50={:.1f} spin={:.0f}% "
        "critical={} threads={} session={} reports={} errors={} pe={} "
        "headroom={:.2f} status={} cap={} self_cpu={:.1f}ms audio={} "
        "audio_threads={} audio_busy_p50={:.0f}% audio_busy_max={:.0f}% "
        "audio_reports={} audio_errors={}",
        mode_, seconds > 0 ? window_frames_ / seconds : 0.0, kTierFps[tier_],
        double(target_ns_) / kNsPerMs, net_p50, net_p90, net_max, raw_p50,
        window_cpu_ns_ ? 100.0 * double(window_spin_ns_) / window_cpu_ns_
                       : 0.0,
        busiest ? busiest->name : std::string("-"), threads_.size(),
        session_ != nullptr, window_reports_, window_errors_,
        power_efficiency_, headroom_, thermal_status_,
        recommended_cap.load(std::memory_order_relaxed), self_ms,
        audio_session_ != nullptr, audio_threads_.size(), audio_p50,
        audio_max, window_audio_reports_, window_audio_errors_);
    ResetWindow();
  }

  void ResetWindow() {
    window_raw_ms_.clear();
    window_net_ms_.clear();
    window_frames_ = 0;
    window_cpu_ns_ = 0;
    window_spin_ns_ = 0;
    window_reports_ = 0;
    window_errors_ = 0;
    window_audio_util_.clear();
    window_audio_reports_ = 0;
    window_audio_errors_ = 0;
    for (Tracked& thread : threads_) {
      thread.critical_frames = 0;
    }
  }

  Ndk ndk_;
  HintManager* manager_ = nullptr;
  HintSession* session_ = nullptr;
  ThermalManager* thermal_ = nullptr;
  int mode_ = 0;
  int64_t target_ns_ = 0;
  bool power_efficiency_ = false;
  int tier_ = 2;
  int tier_candidate_ = 2;
  int tier_streak_ = 0;
  float headroom_ = NAN;
  int thermal_status_ = -1;
  std::chrono::steady_clock::time_point raise_since_;
  std::vector<Tracked> threads_;
  uint32_t seen_generation_ = 0;
  uint32_t last_frame_ = 0;
  std::chrono::steady_clock::time_point last_frame_time_;
  std::chrono::steady_clock::time_point next_housekeeping_;
  std::chrono::steady_clock::time_point next_scan_;
  std::chrono::steady_clock::time_point next_log_;
  std::chrono::steady_clock::time_point log_start_;
  uint64_t self_cpu_base_ = 0;
  std::vector<double> window_raw_ms_;
  std::vector<double> window_net_ms_;
  uint64_t window_frames_ = 0;
  uint64_t window_cpu_ns_ = 0;
  uint64_t window_spin_ns_ = 0;
  uint32_t window_reports_ = 0;
  uint32_t window_errors_ = 0;
  HintSession* audio_session_ = nullptr;
  std::vector<Tracked> audio_threads_;
  int audio_dispatch_ = -2;
  int64_t audio_target_ns_ = 0;
  std::chrono::steady_clock::time_point audio_last_;
  std::chrono::steady_clock::time_point next_audio_scan_;
  std::vector<double> window_audio_util_;
  uint32_t window_audio_reports_ = 0;
  uint32_t window_audio_errors_ = 0;
  std::chrono::steady_clock::time_point next_spin_log_;
};

}  // namespace

void StartAdpf() {
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      Adpf adpf;
      adpf.Run();
    }).detach();
  });
}

void AdpfSetEmulator(xe::Emulator* emulator) {
  std::lock_guard<std::mutex> lock(emulator_mutex);
  emulator_ptr = emulator;
}

int AdpfRecommendedFpsCap() {
  return recommended_cap.load(std::memory_order_relaxed);
}

}  // namespace xdroid
