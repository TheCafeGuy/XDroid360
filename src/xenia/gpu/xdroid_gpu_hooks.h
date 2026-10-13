/**
 * XDroid360: optional callbacks from the GPU and video paths into
 * title-specific code in xdroid-core (HUD-1 ordering arms and forensics).
 * Each is null unless installed; they are set once at title launch.
 */

#ifndef XENIA_GPU_XDROID_GPU_HOOKS_H_
#define XENIA_GPU_XDROID_GPU_HOOKS_H_

#include <atomic>
#include <cstdint>

namespace xe {
namespace gpu {

struct XdroidGpuHooks {
  // Guest thread: the ring buffer write pointer moved (a kick).
  std::atomic<void (*)(uint32_t write_ptr)> on_kick{nullptr};
  // Command processor thread, before executing commands up to |write_ptr|.
  // May block for a bounded time.
  std::atomic<void (*)(uint32_t write_ptr)> before_execute{nullptr};
  // Guest thread, at VdSwap entry. May yield for a bounded time.
  std::atomic<void (*)()> on_guest_swap{nullptr};
  // GPU thread: the glitch recorder flagged log frame |frame|.
  std::atomic<void (*)(uint32_t frame)> on_glitch{nullptr};
};

XdroidGpuHooks& GetXdroidGpuHooks();

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_XDROID_GPU_HOOKS_H_
