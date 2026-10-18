/**
 * XDroid360: optional callbacks from the GPU and video paths into
 * title-specific code in xdroid-core (HUD-1 ordering arms and forensics).
 * Each is null unless installed; they are set once at title launch.
 */

#ifndef XENIA_GPU_XDROID_GPU_HOOKS_H_
#define XENIA_GPU_XDROID_GPU_HOOKS_H_

#include <atomic>
#include <cstdint>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
class Memory;
namespace gpu {

struct XdroidGpuHooks {
  // Guest thread: the ring buffer write pointer moved (a kick).
  std::atomic<void (*)(uint32_t write_ptr)> on_kick{nullptr};
  // Command processor thread, before executing commands up to |write_ptr|.
  // May block for a bounded time.
  std::atomic<void (*)(uint32_t write_ptr)> before_execute{nullptr};
  // Guest thread, at VdSwap entry. May yield for a bounded time.
  std::atomic<void (*)()> on_guest_swap{nullptr};
  // Command processor thread, executing a guest swap (LAT-1).
  std::atomic<void (*)()> on_cp_swap{nullptr};
  // LAT-1: time the CP spent in unmatched PM4_WAIT_REG_MEM, and the address
  // of the longest wait since the last swap (bit 31 = register).
  std::atomic<uint64_t> cp_wait_ns{0};
  std::atomic<uint32_t> cp_wait_longest_addr{0};
  std::atomic<uint64_t> cp_wait_longest_ns{0};
  // GPU thread: the glitch recorder flagged log frame |frame|.
  std::atomic<void (*)(uint32_t frame)> on_glitch{nullptr};
  // MC-1 mark clip: log frame of the latest user mark (0 = none). The glitch
  // recorder saves its last 6 s of frames to <storage>/clips/clip-f<frame>/
  // once per new value, about 1 s after the mark.
  std::atomic<uint64_t> clip_request_frame{0};
  // HUD-1 F0/F1 (temp): guest thread, in UpdateWritePointer before
  // |new_wptr| is published; the new commands are ring dwords
  // [old_wptr, new_wptr) of the primary buffer at |ring_ptr|.
  std::atomic<void (*)(Memory* memory, uint32_t ring_ptr, uint32_t ring_dwords,
                       uint32_t old_wptr, uint32_t new_wptr)>
      before_publish{nullptr};
  // CP thread, entering an indirect buffer: a host copy to execute instead of
  // guest memory, or null. |flags| receives XdroidIbFlags.
  std::atomic<const void* (*)(uint32_t ptr, uint32_t dwords, uint32_t* flags)>
      ib_source{nullptr};
  // CP thread, leaving the indirect buffer entered last.
  std::atomic<void (*)()> ib_end{nullptr};
  // EVT-0: a detector found a problem. |type| is a short stable name, |sig|
  // tells occurrences of one type apart. Called first with |text| null to
  // count; returns true when the event is new for this session, and the
  // caller then calls again with the details. Severity 2+ auto-marks (the
  // capture of a user mark). Any thread; lock-free, never blocks.
  std::atomic<bool (*)(const char* type, int severity, uint64_t sig,
                       const char* text)>
      on_event{nullptr};
};

// Flags of an indirect buffer as the CP executes it (ib_source).
enum XdroidIbFlags : uint32_t {
  kXdroidIbKicked = 1,    // found among the buffers seen at kick
  kXdroidIbSnapshot = 2,  // executed from the copy taken at kick
  kXdroidIbChanged = 4,   // guest bytes at parse differ from those at kick
};

XdroidGpuHooks& GetXdroidGpuHooks();

// EVT-0 helper: counts the event and formats its details only when it is new.
template <typename... Args>
inline void XdroidEvent(const char* type, int severity, uint64_t sig,
                        fmt::format_string<Args...> format, Args&&... args) {
  auto on_event = GetXdroidGpuHooks().on_event.load(std::memory_order_acquire);
  if (on_event && on_event(type, severity, sig, nullptr)) {
    on_event(type, severity, sig,
             fmt::format(format, std::forward<Args>(args)...).c_str());
  }
}

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_XDROID_GPU_HOOKS_H_
