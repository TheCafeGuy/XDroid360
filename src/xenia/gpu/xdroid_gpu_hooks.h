/**
 * XDroid360: optional callbacks from the GPU and video paths into
 * title-specific code in xdroid-core (HUD-1 ordering arms and forensics).
 * Each is null unless installed; they are set once at title launch.
 */

#ifndef XENIA_GPU_XDROID_GPU_HOOKS_H_
#define XENIA_GPU_XDROID_GPU_HOOKS_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
class Memory;
namespace gpu {
class Shader;

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
  // Guest thread, under the global lock: the first write to a page the GPU
  // side holds a copy of since that copy was made. For a forensic probe of
  // who writes a range, and when.
  std::atomic<void (*)(uint32_t physical_address)> on_guest_write{nullptr};
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
  // EVT-0: a detector found a problem. |type| is a short stable name, |sig|
  // tells occurrences of one type apart. Called first with |text| null to
  // count; returns true when the event is new for this session, and the
  // caller then calls again with the details. Severity 2+ auto-marks (the
  // capture of a user mark). Any thread; lock-free, never blocks.
  std::atomic<bool (*)(const char* type, int severity, uint64_t sig,
                       const char* text)>
      on_event{nullptr};
  // BLACK-H1: translation thread, pure. Fills one byte per ucode instruction
  // address: bits 0/1 = vector product operand 0/1 may be inf/NaN, bits 2/3 =
  // scalar product operand 0/1 (ZeroIfAnyOperandIsZero argument order). Null
  // = all may be.
  std::atomic<void (*)(const Shader& shader, std::vector<uint8_t>& out)>
      nonfinite_operands{nullptr};
  // AUTO-1 pad script, per "Frame stats" window: frame << 32 | passes x100.
  std::atomic<uint64_t> frame_stats{0};
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

// F1 XDROID-LAZY: lazy host work that may block the CP thread (G2-3 stall).
enum class XdroidLazyKind : uint32_t {
  kRenderTarget,   // CreateRenderTarget
  kWide,           // TILE-1 wide render target (includes its kRenderTarget)
  kRenderPass,     // render pass cache miss
  kFramebuffer,    // framebuffer cache miss
  kTransfer,       // GetTransferPipelines miss
  kDump,           // GetDumpPipeline miss
  kTexture,        // texture image create and allocate
  kWait,           // unmatched PM4_WAIT_REG_MEM
  kTextureMemory,  // vkAllocateMemory by VMA (inside kTexture)
  kRenderTargetMemory,  // render target image + memory (inside kRenderTarget)
  kCount,
};

// Counts a call over 1 ms in the 10 s sums and logs it (25 lines per kind
// per process; kWait only from 50 ms, as vblank waits are routine). Lock-free,
// any thread. |bytes| != 0 adds the allocation size to the line.
void XdroidLazyNote(XdroidLazyKind kind, uint64_t ns, uint64_t bytes = 0);
// CP thread, per stats window: one sum line per 10 s with nonzero kinds.
void XdroidLazyFlush();

// Times its scope; put it on cache-miss paths only (two clock reads).
class XdroidLazyTimer {
 public:
  explicit XdroidLazyTimer(XdroidLazyKind kind)
      : kind_(kind), begin_(std::chrono::steady_clock::now()) {}
  ~XdroidLazyTimer() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin_);
    XdroidLazyNote(kind_, uint64_t(ns.count()));
  }
  XdroidLazyTimer(const XdroidLazyTimer&) = delete;
  XdroidLazyTimer& operator=(const XdroidLazyTimer&) = delete;

 private:
  XdroidLazyKind kind_;
  std::chrono::steady_clock::time_point begin_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_XDROID_GPU_HOOKS_H_
