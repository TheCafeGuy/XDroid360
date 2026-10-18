#include "xdroid_events.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/xdroid_gpu_hooks.h"

DECLARE_bool(xdroid_diagnostics);

DEFINE_bool(xdroid_auto_mark, true,
            "EVT-0: a new detector event of severity 2 or more saves the same "
            "clip as a user mark (at most one every 30 s, 10 per session).",
            "XDroid360");

namespace xdroid {

namespace {

constexpr uint32_t kSlotCount = 1024;
constexpr int64_t kAutoMarkSpacingUs = 30 * 1000 * 1000;
// Leaves most of the 30 clips a session saves to user marks.
constexpr uint32_t kMaxAutoMarks = 10;

struct Slot {
  // 0 = free.
  std::atomic<uint64_t> key{0};
  std::atomic<const char*> type{nullptr};
  std::atomic<uint64_t> sig{0};
  std::atomic<uint32_t> count{0};
  // Only the summary thread touches it.
  uint32_t logged_count = 0;
};

std::array<Slot, kSlotCount> g_slots;
std::atomic<uint64_t> g_dropped{0};

std::atomic<int64_t> g_last_auto_us{INT64_MIN / 2};
std::atomic<uint32_t> g_auto_marks{0};
// Written only by the event that wins g_last_auto_us (30 s apart), read after
// g_auto_frame is acquired.
char g_auto_text[512];
std::atomic<uint64_t> g_auto_frame{0};

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint64_t Key(const char* type, uint64_t sig) {
  // FNV-1a over the type name, mixed with the signature.
  uint64_t hash = 0xCBF29CE484222325ull;
  for (const char* c = type; *c; ++c) {
    hash = (hash ^ uint8_t(*c)) * 0x100000001B3ull;
  }
  hash ^= sig + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
  return hash ? hash : 1;
}

Slot* FindOrInsert(const char* type, uint64_t sig) {
  const uint64_t key = Key(type, sig);
  for (uint32_t probe = 0; probe < kSlotCount; ++probe) {
    Slot& slot = g_slots[(key + probe) & (kSlotCount - 1)];
    uint64_t current = slot.key.load(std::memory_order_acquire);
    if (current == 0) {
      if (slot.key.compare_exchange_strong(current, key,
                                           std::memory_order_acq_rel)) {
        slot.sig.store(sig, std::memory_order_relaxed);
        slot.type.store(type, std::memory_order_release);
        return &slot;
      }
    }
    if (current == key) {
      return &slot;
    }
  }
  return nullptr;
}

void MaybeAutoMark(const char* type, int severity, uint64_t sig,
                   const char* text, uint64_t frame) {
  if (severity < 2 || !cvars::xdroid_auto_mark ||
      g_auto_marks.load(std::memory_order_relaxed) >= kMaxAutoMarks) {
    return;
  }
  const int64_t now = NowUs();
  int64_t last = g_last_auto_us.load(std::memory_order_relaxed);
  if (now - last < kAutoMarkSpacingUs ||
      !g_last_auto_us.compare_exchange_strong(last, now,
                                              std::memory_order_relaxed)) {
    return;
  }
  if (g_auto_frame.load(std::memory_order_acquire)) {
    // The previous one was never taken (no clip thread): keep it.
    return;
  }
  g_auto_marks.fetch_add(1, std::memory_order_relaxed);
  std::snprintf(g_auto_text, sizeof(g_auto_text),
                "%s sev%d frame %llu sig %016llX %s", type, severity,
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(sig), text);
  g_auto_frame.store(std::max<uint64_t>(frame, 1), std::memory_order_release);
}

bool OnEvent(const char* type, int severity, uint64_t sig, const char* text) {
  if (!cvars::xdroid_diagnostics) {
    return false;
  }
  Slot* slot = FindOrInsert(type, sig);
  if (!slot) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!text) {
    return slot->count.fetch_add(1, std::memory_order_relaxed) == 0;
  }
  const uint64_t frame = xe::logging::GetFrameNumber();
  XELOGI("XDROID-EVT {} sev{} frame {} sig {:016X} {}", type, severity, frame,
         sig, text);
  MaybeAutoMark(type, severity, sig, text, frame);
  return false;
}

}  // namespace

void InstallEventBus() {
  static std::once_flag installed;
  std::call_once(installed, [] {
    xe::gpu::GetXdroidGpuHooks().on_event.store(OnEvent,
                                                std::memory_order_release);
  });
}

bool TakeAutoMark(uint64_t& frame, std::string& text) {
  frame = g_auto_frame.load(std::memory_order_acquire);
  if (!frame) {
    return false;
  }
  text.assign(g_auto_text);
  g_auto_frame.store(0, std::memory_order_release);
  return true;
}

void LogEventSummary() {
  for (Slot& slot : g_slots) {
    const char* type = slot.type.load(std::memory_order_acquire);
    if (!type) {
      continue;
    }
    const uint32_t count = slot.count.load(std::memory_order_relaxed);
    if (count == slot.logged_count) {
      continue;
    }
    XELOGI("XDROID-EVT-SUM {} sig {:016X} count {} (+{})", type,
           slot.sig.load(std::memory_order_relaxed), count,
           count - slot.logged_count);
    slot.logged_count = count;
  }
  static uint64_t logged_dropped = 0;
  const uint64_t dropped = g_dropped.load(std::memory_order_relaxed);
  if (dropped != logged_dropped) {
    logged_dropped = dropped;
    XELOGW("XDROID-EVT-SUM table full, {} events not counted", dropped);
  }
}

}  // namespace xdroid
