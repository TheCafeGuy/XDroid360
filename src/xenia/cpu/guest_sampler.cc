/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_sampler.h"

#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"

DEFINE_uint32(guest_sampler_hz, 0,
              "XDroid360 CPU sampler: samples per second of CPU time on each "
              "guest and emulator thread (0 = off). Live. The kernel fires "
              "CPU-time timers on its scheduler tick, so rates above "
              "CONFIG_HZ (250 on most phones) give 250. Reports every "
              "guest_sampler_report_s seconds as XDROID-SAMPLER log lines.",
              "CPU");
DEFINE_uint32(guest_sampler_report_s, 10,
              "Seconds of samples per XDROID-SAMPLER report.", "CPU");
DEFINE_string(guest_sampler_spin_ranges, "",
              "Extra guest address ranges the sampler counts as spin-waits, "
              "e.g. \"821E6200-821E6C00,82300000-82300100\". Code within 256 "
              "bytes of a db16cyc hint in the same function counts already.",
              "CPU");

#if XE_PLATFORM_LINUX || XE_PLATFORM_ANDROID

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {
namespace sampler {
namespace {

// Code this close to a db16cyc hint, in the same function, is the spin loop.
constexpr uint32_t kSpinHintReach = 0x100;
constexpr uint32_t kRingSize = 2048;  // power of two
constexpr int kMaxSlots = 128;
constexpr auto kDrainPeriod = std::chrono::milliseconds(100);

struct Sample {
  uint64_t pc;
  // Return address of the last guest->host call (PPCContext::host_call_return)
  // when one is known, else the interrupted LR.
  uint64_t caller;
  uint32_t guest_thread_id;
};

// One per registered host thread. The signal handler is the only writer of the
// ring head; the sampler thread is the only reader.
struct Slot {
  char name[32] = {};
  clockid_t cpu_clock = 0;
  timer_t timer = {};
  bool has_timer = false;
  uint32_t armed_hz = 0;  // sampler thread only
  bool dead = false;      // sampler thread only
  std::atomic<uint32_t> guest_thread_id{0};
  std::atomic<ppc::PPCContext*> context{nullptr};
  std::atomic<uint32_t> head{0};
  std::atomic<uint32_t> tail{0};
  std::atomic<uint32_t> dropped{0};
  Sample ring[kRingSize];
  // Window state, sampler thread only.
  uint64_t window_cpu_start_ns = 0;
};

std::atomic<Slot*> slots[kMaxSlots];
std::atomic<int> slot_count{0};
std::atomic<Processor*> processor_{nullptr};
thread_local Slot* current_slot = nullptr;

std::mutex spin_hints_mutex;
std::set<uint32_t> spin_hints;

int SampleSignal() {
  // threading_posix.cc takes SIGRTMIN + 0..2 for suspend, user callbacks and
  // (Android) terminate. SIGRTMIN already excludes the libc-reserved ones.
  return SIGRTMIN + 4;
}

void SampleSignalHandler(int, siginfo_t* info, void* raw_ucontext) {
  const int saved_errno = errno;
  auto* slot = static_cast<Slot*>(info->si_value.sival_ptr);
  if (slot && info->si_code == SI_TIMER) {
    auto* uc = static_cast<ucontext_t*>(raw_ucontext);
    Sample sample;
#if XE_ARCH_ARM64
    sample.pc = uc->uc_mcontext.pc;
    sample.caller = uc->uc_mcontext.regs[30];
#elif XE_ARCH_AMD64
    sample.pc = uint64_t(uc->uc_mcontext.gregs[REG_RIP]);
    sample.caller = 0;
#else
    sample.pc = 0;
    sample.caller = 0;
#endif
    sample.guest_thread_id =
        slot->guest_thread_id.load(std::memory_order_relaxed);
    ppc::PPCContext* context = slot->context.load(std::memory_order_relaxed);
    if (context && context->host_call_return) {
      sample.caller = context->host_call_return;
    }
    const uint32_t head = slot->head.load(std::memory_order_relaxed);
    if (head - slot->tail.load(std::memory_order_acquire) >= kRingSize) {
      slot->dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
      slot->ring[head & (kRingSize - 1)] = sample;
      slot->head.store(head + 1, std::memory_order_release);
    }
  }
  errno = saved_errno;
}

uint64_t ReadCpuNs(const Slot& slot, bool* ok) {
  timespec ts;
  if (clock_gettime(slot.cpu_clock, &ts)) {
    *ok = false;
    return 0;
  }
  *ok = true;
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void Arm(Slot& slot, uint32_t hz) {
  if (!slot.has_timer || slot.armed_hz == hz) {
    return;
  }
  itimerspec spec = {};
  if (hz) {
    const long interval_ns = long(1000000000ull / hz);
    spec.it_interval.tv_sec = interval_ns / 1000000000L;
    spec.it_interval.tv_nsec = interval_ns % 1000000000L;
    spec.it_value = spec.it_interval;
  }
  // A thread that exited takes its CPU clock with it.
  if (timer_settime(slot.timer, 0, &spec, nullptr)) {
    slot.dead = true;
    return;
  }
  slot.armed_hz = hz;
}

std::vector<std::pair<uint32_t, uint32_t>> ParseSpinRanges(
    const std::string& text) {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find(',', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string item = text.substr(pos, end - pos);
    size_t dash = item.find('-');
    if (dash != std::string::npos) {
      char* tail = nullptr;
      uint32_t first = uint32_t(std::strtoul(item.c_str(), &tail, 16));
      uint32_t last =
          uint32_t(std::strtoul(item.c_str() + dash + 1, &tail, 16));
      if (first && last > first) {
        ranges.emplace_back(first, last);
      }
    }
    pos = end + 1;
  }
  return ranges;
}

enum class Kind : uint8_t {
  kJit,       // translated guest code
  kSpin,      // translated guest code inside a spin-wait
  kHelper,    // JIT helpers and thunks (guest->host transitions)
  kHost,      // emulator C++ (kernel exports, MMIO, LoadClock...) for a guest
  kHostSpin,  // host or helper time entered from a spin-wait
  kNoGuest,   // emulator C++ with no guest thread on the host thread
  kCount,
};
struct Resolved {
  Kind kind = Kind::kNoGuest;
  uint32_t guest_address = 0;   // guest instruction, or the guest call site
  uint32_t guest_function = 0;  // its function
};

class Resolver {
 public:
  explicit Resolver(Processor* processor)
      : code_cache_(processor && processor->backend()
                        ? processor->backend()->code_cache()
                        : nullptr) {
    if (code_cache_) {
      code_begin_ = code_cache_->execute_base_address();
      code_end_ = code_begin_ + code_cache_->total_size();
    }
    {
      std::lock_guard<std::mutex> lock(spin_hints_mutex);
      hints_.assign(spin_hints.begin(), spin_hints.end());
    }
    ranges_ = ParseSpinRanges(cvars::guest_sampler_spin_ranges);
  }

  bool InCode(uint64_t pc) const {
    return code_cache_ && pc >= code_begin_ && pc < code_end_;
  }

  struct GuestPlace {
    uint32_t address = 0;   // guest instruction
    uint32_t function = 0;  // start of its function
    uint32_t end = 0;       // end of its function, 0 if unknown
  };

  // Guest place of a code cache address; address is 0 for a helper or thunk.
  const GuestPlace& MapGuest(uint64_t pc) {
    auto it = guest_cache_.find(pc);
    if (it == guest_cache_.end()) {
      GuestPlace place;
      GuestFunction* fn = code_cache_->LookupFunction(pc);
      if (fn) {
        place.function = fn->address();
        place.end = fn->end_address();
        place.address = fn->MapMachineCodeToGuestAddress(uintptr_t(pc));
        // An address the source map cannot place still belongs to fn.
        if (!place.address) {
          place.address = place.function;
        }
      }
      it = guest_cache_.emplace(pc, place).first;
    }
    return it->second;
  }

  bool IsSpin(const GuestPlace& place) const {
    for (const auto& range : ranges_) {
      if (place.address >= range.first && place.address < range.second) {
        return true;
      }
    }
    // A hint within reach, inside the same function (a neighbour's hint does
    // not make this code a spin-wait).
    const uint32_t low = std::max(
        place.function,
        place.address > kSpinHintReach ? place.address - kSpinHintReach : 0);
    uint32_t high = place.address + kSpinHintReach;
    if (place.end) {
      high = std::min(high, place.end);
    }
    auto it = std::lower_bound(hints_.begin(), hints_.end(), low);
    return it != hints_.end() && *it <= high;
  }

  Resolved Resolve(const Sample& sample, bool has_guest) {
    Resolved out;
    if (InCode(sample.pc)) {
      const GuestPlace& place = MapGuest(sample.pc);
      if (place.address) {
        out.kind = IsSpin(place) ? Kind::kSpin : Kind::kJit;
        out.guest_address = place.address;
        out.guest_function = place.function;
        return out;
      }
      out.kind = Kind::kHelper;
    } else {
      out.kind = has_guest ? Kind::kHost : Kind::kNoGuest;
    }
    if (has_guest && InCode(sample.caller)) {
      const GuestPlace& place = MapGuest(sample.caller);
      if (place.address) {
        out.guest_address = place.address;
        out.guest_function = place.function;
        if (IsSpin(place)) {
          out.kind = Kind::kHostSpin;
        }
      }
    }
    return out;
  }

 private:
  backend::CodeCache* code_cache_;
  uint64_t code_begin_ = 0;
  uint64_t code_end_ = 0;
  std::vector<uint32_t> hints_;
  std::vector<std::pair<uint32_t, uint32_t>> ranges_;
  std::unordered_map<uint64_t, GuestPlace> guest_cache_;
};

struct ThreadWindow {
  uint64_t samples = 0;
  std::unordered_map<uint32_t, uint32_t> guest_threads;
  // Classified at report time, when the code cache is walked once.
  std::vector<Sample> raw;
};

std::string HostName(uint64_t pc) {
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fbase) {
    const char* file = info.dli_fname ? info.dli_fname : "?";
    const char* base = std::strrchr(file, '/');
    std::string out = base ? base + 1 : file;
    out += fmt::format("+0x{:X}", pc - uint64_t(info.dli_fbase));
    if (info.dli_sname) {
      out += fmt::format("({})", info.dli_sname);
    }
    return out;
  }
  return fmt::format("0x{:X}", pc);
}

void Report(std::vector<ThreadWindow>& windows, double seconds, uint32_t hz) {
  Resolver resolver(processor_.load());
  struct FunctionStat {
    uint32_t samples = 0;
    uint32_t spin = 0;
    uint32_t host = 0;
  };
  double total_cpu = 0, total_spin_cpu = 0;
  uint64_t total_samples = 0, total_dropped = 0;
  const int count = std::min(slot_count.load(), kMaxSlots);
  std::vector<std::string> lines;
  for (int i = 0; i < count; ++i) {
    Slot* slot = slots[i].load();
    ThreadWindow& window = windows[i];
    if (!slot || slot->dead) {
      continue;
    }
    bool ok;
    const uint64_t cpu_ns = ReadCpuNs(*slot, &ok);
    const double cpu_pct = ok ? 100.0 *
                                    double(cpu_ns - slot->window_cpu_start_ns) /
                                    (seconds * 1e9)
                              : 0.0;
    slot->window_cpu_start_ns = cpu_ns;
    total_dropped += slot->dropped.exchange(0);
    if (!window.samples) {
      if (cpu_pct >= 5.0) {
        lines.push_back(
            fmt::format("  '{}' cpu={:.0f}% no samples", slot->name, cpu_pct));
      }
      window = ThreadWindow();
      continue;
    }
    total_samples += window.samples;
    // Classify every distinct sample once.
    uint64_t by_kind[size_t(Kind::kCount)] = {};
    std::unordered_map<uint32_t, FunctionStat> functions;
    std::unordered_map<uint64_t, uint32_t> host_pcs;
    std::unordered_map<uint32_t, uint32_t> guest_pcs;
    for (const Sample& sample : window.raw) {
      Resolved r = resolver.Resolve(sample, sample.guest_thread_id != 0);
      ++by_kind[size_t(r.kind)];
      if (r.guest_function) {
        FunctionStat& stat = functions[r.guest_function];
        ++stat.samples;
        if (r.kind == Kind::kSpin || r.kind == Kind::kHostSpin) {
          ++stat.spin;
        }
        if (r.kind != Kind::kJit && r.kind != Kind::kSpin) {
          ++stat.host;
        }
      }
      if (r.kind != Kind::kJit && r.kind != Kind::kSpin) {
        ++host_pcs[resolver.InCode(sample.pc) ? 0 : sample.pc];
      } else {
        ++guest_pcs[r.guest_address];
      }
    }
    const double n = double(window.samples);
    auto pct = [n](uint64_t v) { return 100.0 * double(v) / n; };
    const uint64_t spin =
        by_kind[size_t(Kind::kSpin)] + by_kind[size_t(Kind::kHostSpin)];
    total_cpu += cpu_pct;
    total_spin_cpu += cpu_pct * double(spin) / n;
    lines.push_back(fmt::format(
        "  '{}' cpu={:.0f}% samples={} spin={:.0f}% (work cpu={:.0f}%) "
        "jit={:.0f}% helper={:.0f}% host={:.0f}% noguest={:.0f}%",
        slot->name, cpu_pct, window.samples, pct(spin),
        cpu_pct * (1.0 - double(spin) / n), pct(by_kind[size_t(Kind::kJit)]),
        pct(by_kind[size_t(Kind::kHelper)]), pct(by_kind[size_t(Kind::kHost)]),
        pct(by_kind[size_t(Kind::kNoGuest)])));
    // Guest threads sharing this host thread (fibers).
    if (window.guest_threads.size() > 1) {
      std::vector<std::pair<uint32_t, uint32_t>> guests(
          window.guest_threads.begin(), window.guest_threads.end());
      std::sort(guests.begin(), guests.end(),
                [](auto& a, auto& b) { return a.second > b.second; });
      std::string text = "    guest threads:";
      for (size_t g = 0; g < guests.size() && g < 6; ++g) {
        text += fmt::format(" {:08X}={:.0f}%", guests[g].first,
                            pct(guests[g].second));
      }
      lines.push_back(std::move(text));
    }
    std::vector<std::pair<uint32_t, FunctionStat>> top(functions.begin(),
                                                       functions.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) {
      return a.second.samples > b.second.samples;
    });
    std::string text = "    functions:";
    for (size_t f = 0; f < top.size() && f < 8; ++f) {
      text += fmt::format(" {:08X}={:.1f}%", top[f].first,
                          pct(top[f].second.samples));
      if (top[f].second.spin * 2 > top[f].second.samples) {
        text += "[spin]";
      }
      if (top[f].second.host * 2 > top[f].second.samples) {
        text += "[host]";
      }
    }
    if (!top.empty()) {
      lines.push_back(std::move(text));
    }
    std::vector<std::pair<uint32_t, uint32_t>> pcs(guest_pcs.begin(),
                                                   guest_pcs.end());
    std::sort(pcs.begin(), pcs.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    if (!pcs.empty()) {
      text = "    guest pcs:";
      for (size_t p = 0; p < pcs.size() && p < 8; ++p) {
        text +=
            fmt::format(" {:08X}={:.1f}%", pcs[p].first, pct(pcs[p].second));
      }
      lines.push_back(std::move(text));
    }
    std::vector<std::pair<uint64_t, uint32_t>> hosts(host_pcs.begin(),
                                                     host_pcs.end());
    std::sort(hosts.begin(), hosts.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    if (!hosts.empty()) {
      text = "    host:";
      for (size_t h = 0; h < hosts.size() && h < 5; ++h) {
        text += fmt::format(" {}={:.1f}%",
                            hosts[h].first ? HostName(hosts[h].first)
                                           : std::string("jit-helpers"),
                            pct(hosts[h].second));
      }
      lines.push_back(std::move(text));
    }
    window = ThreadWindow();
  }
  XELOGI(
      "XDROID-SAMPLER {:.1f}s @{}Hz samples={} dropped={} cpu={:.0f}% "
      "spin cpu={:.0f}% work cpu={:.0f}%",
      seconds, hz, total_samples, total_dropped, total_cpu, total_spin_cpu,
      total_cpu - total_spin_cpu);
  for (const std::string& line : lines) {
    XELOGI("XDROID-SAMPLER{}", line);
  }
}

void SamplerLoop() {
  std::vector<ThreadWindow> windows(kMaxSlots);
  uint32_t hz = 0;
  auto window_start = std::chrono::steady_clock::now();
  while (true) {
    const uint32_t want_hz = std::min<uint32_t>(cvars::guest_sampler_hz, 10000);
    const int count = std::min(slot_count.load(), kMaxSlots);
    if (want_hz != hz) {
      hz = want_hz;
      window_start = std::chrono::steady_clock::now();
      for (int i = 0; i < count; ++i) {
        if (Slot* slot = slots[i].load()) {
          bool ok;
          slot->window_cpu_start_ns = ReadCpuNs(*slot, &ok);
          slot->tail.store(slot->head.load());
          windows[i] = ThreadWindow();
        }
      }
    }
    if (!hz) {
      for (int i = 0; i < count; ++i) {
        if (Slot* slot = slots[i].load(); slot && !slot->dead) {
          Arm(*slot, 0);
        }
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    std::this_thread::sleep_for(kDrainPeriod);
    for (int i = 0; i < count; ++i) {
      Slot* slot = slots[i].load();
      if (!slot || slot->dead) {
        continue;
      }
      if (slot->armed_hz != hz) {
        // Joined after the window began: count its CPU from now.
        if (!slot->armed_hz) {
          bool ok;
          slot->window_cpu_start_ns = ReadCpuNs(*slot, &ok);
        }
        Arm(*slot, hz);
      }
      ThreadWindow& window = windows[i];
      uint32_t tail = slot->tail.load(std::memory_order_relaxed);
      const uint32_t head = slot->head.load(std::memory_order_acquire);
      for (; tail != head; ++tail) {
        const Sample& sample = slot->ring[tail & (kRingSize - 1)];
        ++window.samples;
        ++window.guest_threads[sample.guest_thread_id];
        window.raw.push_back(sample);
      }
      slot->tail.store(tail, std::memory_order_release);
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(now - window_start).count();
    if (seconds >= std::max<uint32_t>(cvars::guest_sampler_report_s, 1)) {
      Report(windows, seconds, hz);
      window_start = now;
    }
  }
}

void StartOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction action = {};
    action.sa_sigaction = SampleSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SampleSignal(), &action, nullptr)) {
      XELOGE("XDROID-SAMPLER: sigaction failed ({}), sampler unavailable",
             errno);
      return;
    }
    std::thread(SamplerLoop).detach();
  });
}

}  // namespace

void RegisterCurrentThread(Processor* processor, const char* name) {
  if (current_slot) {
    return;
  }
  StartOnce();
  const int index = slot_count.fetch_add(1);
  if (index >= kMaxSlots) {
    if (index == kMaxSlots) {
      XELOGW("XDROID-SAMPLER: more than {} threads, not sampling '{}' on",
             kMaxSlots, name ? name : "");
    }
    return;
  }
  if (processor) {
    processor_.store(processor);
  }
  auto* slot = new Slot();
  std::strncpy(slot->name, name ? name : "", sizeof(slot->name) - 1);
  if (pthread_getcpuclockid(pthread_self(), &slot->cpu_clock)) {
    slot->dead = true;
  }
  sigevent event = {};
  event.sigev_notify = SIGEV_THREAD_ID;
  event.sigev_signo = SampleSignal();
  event.sigev_value.sival_ptr = slot;
#ifdef sigev_notify_thread_id
  event.sigev_notify_thread_id = pid_t(syscall(SYS_gettid));
#else
  event._sigev_un._tid = pid_t(syscall(SYS_gettid));
#endif
  // The thread's own CPU-time clock: the timer only advances while it runs.
  slot->has_timer = !slot->dead && timer_create(CLOCK_THREAD_CPUTIME_ID, &event,
                                                &slot->timer) == 0;
  current_slot = slot;
  slots[index].store(slot);
}

void SetCurrentGuestThread(uint32_t guest_thread_id,
                           ppc::PPCContext_s* context) {
  Slot* slot = current_slot;
  if (!slot) {
    return;
  }
  slot->guest_thread_id.store(guest_thread_id, std::memory_order_relaxed);
  slot->context.store(context, std::memory_order_relaxed);
}

void NoteSpinHint(uint32_t guest_address) {
  if (!guest_address) {
    return;
  }
  std::lock_guard<std::mutex> lock(spin_hints_mutex);
  spin_hints.insert(guest_address);
}

}  // namespace sampler
}  // namespace cpu
}  // namespace xe

#else  // XE_PLATFORM_LINUX || XE_PLATFORM_ANDROID

namespace xe {
namespace cpu {
namespace sampler {
void RegisterCurrentThread(Processor*, const char*) {}
void SetCurrentGuestThread(uint32_t, ppc::PPCContext_s*) {}
void NoteSpinHint(uint32_t) {}
}  // namespace sampler
}  // namespace cpu
}  // namespace xe

#endif  // XE_PLATFORM_LINUX || XE_PLATFORM_ANDROID
