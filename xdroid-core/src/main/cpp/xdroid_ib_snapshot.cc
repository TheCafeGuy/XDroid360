/**
 * XDroid360 HUD-1 arms F0/F1 (temp, revert-planned). See xdroid_ib_snapshot.h.
 */

#include "xdroid_ib_snapshot.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/xdroid_gpu_hooks.h"
#include "xenia/memory.h"

DEFINE_bool(xdroid_cp_ib_kick_hash, false,
            "HUD-1 arm F0 (temp): hash every indirect buffer when the guest "
            "kicks it and again when the command processor parses it; log "
            "the buffers that changed in between (XDROID-IBSNAP). Live.",
            "XDroid360");
DEFINE_bool(xdroid_cp_snapshot_ib, false,
            "HUD-1 arm F1 (temp): copy every indirect buffer when the guest "
            "kicks it and let the command processor execute the copy, as a "
            "prefetching Xenos would. Implies the F0 hashes. Live.",
            "XDroid360");

namespace xdroid {
namespace {

constexpr uint32_t kEntries = 8192;
constexpr uint64_t kArenaBytes = 8 * 1024 * 1024;
constexpr uint32_t kMaxDepth = 4;
// Larger buffers are hashed but not copied.
constexpr uint32_t kMaxCopyDwords = 64 * 1024;
// How far past its cursor the CP looks for the buffer it enters.
constexpr uint32_t kLookahead = 64;
constexpr uint32_t kPm4IndirectBuffer = 0x3F;
constexpr uint32_t kPm4IndirectBufferPfd = 0x37;

struct Entry {
  uint32_t ptr;
  uint32_t dwords;
  uint64_t hash;
  uint64_t arena_pos;  // UINT64_MAX: not copied
  uint64_t kick_us;
};

std::array<Entry, kEntries> g_entries;
std::unique_ptr<uint8_t[]> g_arena;
// Producer side.
std::atomic<uint64_t> g_produced{0};
uint64_t g_arena_head = 0;
// Consumer side: next entry to look at, arena bytes no longer needed.
std::atomic<uint64_t> g_consumed{0};
std::atomic<uint64_t> g_arena_free{0};
std::atomic<xe::Memory*> g_memory{nullptr};
// Consumer: arena start of each buffer being executed, innermost last.
std::array<uint64_t, 8> g_exec_arena;
uint32_t g_exec_depth = 0;
uint64_t g_last_end = 0;

// Counters (relaxed; read by the CP's 10 s report).
std::atomic<uint64_t> g_kicks{0}, g_buffers{0}, g_copied{0}, g_drops{0};
uint64_t g_matched = 0, g_misses = 0, g_changed = 0, g_served = 0;
uint64_t g_last_report_us = 0;
uint32_t g_changed_logged = 0;

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

const uint8_t* Guest(xe::Memory* memory, uint32_t ptr) {
  return memory->TranslatePhysical<const uint8_t*>(ptr & 0x1FFFFFFF);
}

uint32_t ReadDword(const uint8_t* base, uint32_t index) {
  uint32_t value;
  std::memcpy(&value, base + size_t(index) * 4, 4);
  return xe::byte_swap(value);
}

void Record(xe::Memory* memory, uint32_t ptr, uint32_t dwords, bool copy);

// Walks |count| dwords at |base| starting at dword |start| (wrapping at
// |wrap| dwords, the ring size; 0 for an indirect buffer) and records the
// indirect buffers they call, depth first, in the order the CP enters them.
void Walk(xe::Memory* memory, const uint8_t* base, uint32_t start,
          uint32_t wrap, uint32_t count, uint32_t depth, bool copy) {
  auto at = [base, start, wrap](uint32_t j) {
    return ReadDword(base, wrap ? (start + j) % wrap : start + j);
  };
  uint32_t i = 0;
  while (i < count) {
    const uint32_t packet = at(i);
    const uint32_t type = packet >> 30;
    uint32_t length;
    if (type == 3) {
      length = ((packet >> 16) & 0x3FFF) + 1;
      const uint32_t opcode = (packet >> 8) & 0x7F;
      if ((opcode == kPm4IndirectBuffer || opcode == kPm4IndirectBufferPfd) &&
          i + 2 < count && depth < kMaxDepth) {
        const uint32_t ptr = at(i + 1) & 0x1FFFFFFF;
        const uint32_t dwords = at(i + 2) & 0xFFFFF;
        if (dwords) {
          Record(memory, ptr, dwords, copy);
          Walk(memory, Guest(memory, ptr), 0, 0, dwords, depth + 1, copy);
        }
      }
    } else if (type == 0) {
      length = ((packet >> 16) & 0x3FFF) + 1;
    } else if (type == 1) {
      length = 2;
    } else {
      length = 0;
    }
    i += 1 + length;
  }
}

void Record(xe::Memory* memory, uint32_t ptr, uint32_t dwords, bool copy) {
  const uint64_t produced = g_produced.load(std::memory_order_relaxed);
  if (produced - g_consumed.load(std::memory_order_acquire) >= kEntries) {
    g_drops.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint8_t* bytes = Guest(memory, ptr);
  const uint32_t length = dwords * 4;
  Entry& entry = g_entries[produced % kEntries];
  entry.ptr = ptr;
  entry.dwords = dwords;
  entry.hash = XXH3_64bits(bytes, length);
  entry.kick_us = NowUs();
  entry.arena_pos = UINT64_MAX;
  if (copy && dwords <= kMaxCopyDwords) {
    uint64_t pos = g_arena_head;
    // Contiguous copies: skip the arena tail when it does not fit.
    if (pos % kArenaBytes + length > kArenaBytes) {
      pos += kArenaBytes - pos % kArenaBytes;
    }
    if (pos + length - g_arena_free.load(std::memory_order_acquire) <=
        kArenaBytes) {
      std::memcpy(g_arena.get() + pos % kArenaBytes, bytes, length);
      entry.arena_pos = pos;
      g_arena_head = pos + length;
      g_copied.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_drops.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_buffers.fetch_add(1, std::memory_order_relaxed);
  g_produced.store(produced + 1, std::memory_order_release);
}

void BeforePublish(xe::Memory* memory, uint32_t ring_ptr, uint32_t ring_dwords,
                   uint32_t old_wptr, uint32_t new_wptr) {
  const bool copy = cvars::xdroid_cp_snapshot_ib;
  if ((!copy && !cvars::xdroid_cp_ib_kick_hash) || !ring_dwords ||
      old_wptr >= ring_dwords || new_wptr >= ring_dwords) {
    return;
  }
  if (copy && !g_arena) {
    g_arena = std::make_unique<uint8_t[]>(kArenaBytes);
  }
  g_memory.store(memory, std::memory_order_release);
  g_kicks.fetch_add(1, std::memory_order_relaxed);
  const uint8_t* ring = Guest(memory, ring_ptr);
  const uint32_t count = (new_wptr + ring_dwords - old_wptr) % ring_dwords;
  Walk(memory, ring, old_wptr, ring_dwords, count, 0, copy);
}

void Report(uint64_t now) {
  if (now - g_last_report_us < 10'000'000) {
    return;
  }
  g_last_report_us = now;
  XELOGI(
      "XDROID-IBSNAP: kicks {} buffers {} copied {} dropped {}; CP matched {} "
      "missed {} changed since kick {} executed from copy {}",
      g_kicks.load(std::memory_order_relaxed),
      g_buffers.load(std::memory_order_relaxed),
      g_copied.load(std::memory_order_relaxed),
      g_drops.load(std::memory_order_relaxed), g_matched, g_misses, g_changed,
      g_served);
  g_changed_logged = 0;
}

const void* IbSource(uint32_t ptr, uint32_t dwords, uint32_t* flags) {
  *flags = 0;
  const bool copy = cvars::xdroid_cp_snapshot_ib;
  xe::Memory* memory = g_memory.load(std::memory_order_acquire);
  uint64_t arena_pos = UINT64_MAX;
  const void* source = nullptr;
  if (!copy && !cvars::xdroid_cp_ib_kick_hash) {
    // Arms off: drop what was recorded so a later switch-on never pairs a
    // buffer with an old copy.
    g_consumed.store(g_produced.load(std::memory_order_acquire),
                     std::memory_order_release);
  } else if (memory) {
    const uint64_t now = NowUs();
    Report(now);
    const uint64_t produced = g_produced.load(std::memory_order_acquire);
    const uint64_t consumed = g_consumed.load(std::memory_order_relaxed);
    const Entry* found = nullptr;
    for (uint64_t i = consumed; i < produced && i < consumed + kLookahead;
         ++i) {
      const Entry& entry = g_entries[i % kEntries];
      if (entry.ptr == (ptr & 0x1FFFFFFF) && entry.dwords == dwords &&
          now - entry.kick_us < 500'000) {
        found = &entry;
        g_consumed.store(i + 1, std::memory_order_release);
        break;
      }
    }
    if (found) {
      ++g_matched;
      *flags |= xe::gpu::kXdroidIbKicked;
      arena_pos = found->arena_pos;
      if (arena_pos != UINT64_MAX) {
        g_last_end = arena_pos + uint64_t(dwords) * 4;
      }
      const uint64_t hash = XXH3_64bits(Guest(memory, ptr), size_t(dwords) * 4);
      if (hash != found->hash) {
        ++g_changed;
        *flags |= xe::gpu::kXdroidIbChanged;
        if (g_changed_logged < 40) {
          ++g_changed_logged;
          XELOGW(
              "XDROID-IBSNAP: buffer {:08X} ({} dwords) changed between kick "
              "and parse ({} us later){}",
              ptr, dwords, now - found->kick_us,
              copy && arena_pos != UINT64_MAX ? ", executing the kick copy"
                                              : "");
        }
      }
      if (copy && arena_pos != UINT64_MAX && g_arena) {
        source = g_arena.get() + arena_pos % kArenaBytes;
        *flags |= xe::gpu::kXdroidIbSnapshot;
        ++g_served;
      }
    } else if (produced) {
      ++g_misses;
    }
  }
  if (g_exec_depth < g_exec_arena.size()) {
    g_exec_arena[g_exec_depth] = arena_pos;
  }
  ++g_exec_depth;
  return source;
}

// Copies are appended in arena order. The arena is free up to the outermost
// copy still executing; with none executing, up to the first copy not yet
// entered, or past the last copy entered.
void IbEnd() {
  if (!g_exec_depth) {
    return;
  }
  --g_exec_depth;
  uint64_t keep = UINT64_MAX;
  for (uint32_t d = 0; d < std::min<uint32_t>(g_exec_depth, 8); ++d) {
    keep = std::min(keep, g_exec_arena[d]);
  }
  if (keep == UINT64_MAX) {
    const uint64_t produced = g_produced.load(std::memory_order_acquire);
    for (uint64_t i = g_consumed.load(std::memory_order_relaxed);
         i < produced; ++i) {
      if (g_entries[i % kEntries].arena_pos != UINT64_MAX) {
        keep = g_entries[i % kEntries].arena_pos;
        break;
      }
    }
    if (keep == UINT64_MAX) {
      keep = g_last_end;
    }
  }
  if (keep > g_arena_free.load(std::memory_order_relaxed)) {
    g_arena_free.store(keep, std::memory_order_release);
  }
}

}  // namespace

void InstallIbSnapshotHooks() {
  auto& hooks = xe::gpu::GetXdroidGpuHooks();
  hooks.before_publish.store(BeforePublish, std::memory_order_release);
  hooks.ib_source.store(IbSource, std::memory_order_release);
  hooks.ib_end.store(IbEnd, std::memory_order_release);
}

}  // namespace xdroid
