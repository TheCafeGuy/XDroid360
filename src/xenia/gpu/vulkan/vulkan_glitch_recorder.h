/**
 * XDroid360 forensic tool (IS-2): finds one-frame visual glitches in the
 * guest output and saves them as images, without a person marking them.
 *
 * Every swap the final guest output (after the gamma ramp) is blitted into a
 * ring of small frames on the GPU and into a 64x36 thumbnail that is read back.
 * A frame whose thumbnail differs from both neighbours while the neighbours
 * agree with each other is a one-frame glitch candidate; the three frames
 * around it are copied out of the ring and written as PPM files to
 * <storage>/glitch/, with an XDROID-GLITCH log line carrying the frame number.
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_GLITCH_RECORDER_H_
#define XENIA_GPU_VULKAN_VULKAN_GLITCH_RECORDER_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace ui {
namespace vulkan {
class VulkanDevice;
}  // namespace vulkan
}  // namespace ui
namespace gpu {
namespace vulkan {

class VulkanGlitchRecorder {
 public:
  ~VulkanGlitchRecorder() { Shutdown(); }

  // Lazily creates its resources on the first enabled capture.
  bool EnsureInitialized(const ui::vulkan::VulkanDevice* vulkan_device,
                         const std::filesystem::path& output_dir);
  void Shutdown();
  bool initialized() const { return initialized_; }

  // Reads back finished thumbnails, scores them, and records the copy of a
  // candidate's frames into the open submission. Writes finished dumps.
  void Poll(DeferredCommandBuffer& command_buffer,
            uint64_t completed_submission, uint64_t current_submission,
            uint64_t frame);
  // With the guest output image in GENERAL and every write to it made
  // available to transfer reads (the caller's barrier), records its capture.
  void Capture(DeferredCommandBuffer& command_buffer, VkImage guest_output,
               uint32_t width, uint32_t height, uint64_t current_submission,
               uint64_t frame);
  // The last frame flagged as HUD-1 (radar blocks), UINT64_MAX once taken.
  uint64_t TakeHud1Frame() { return std::exchange(hud1_frame_, UINT64_MAX); }

 private:
  static constexpr uint32_t kRingSize = 8;
  static constexpr uint32_t kFrameWidth = 640;
  static constexpr uint32_t kFrameHeight = 360;
  static constexpr uint32_t kThumbWidth = 64;
  static constexpr uint32_t kThumbHeight = 36;
  static constexpr uint32_t kThumbBytes = kThumbWidth * kThumbHeight * 4;
  static constexpr uint32_t kFrameBytes = kFrameWidth * kFrameHeight * 4;
  static constexpr uint32_t kDumpFrames = 4;  // n-2..n+1
  static constexpr uint32_t kMaxDumps = 200;

  struct Slot {
    uint64_t frame = UINT64_MAX;
    uint64_t submission = UINT64_MAX;
    bool scored = false;
  };

  void TransferBarrier(DeferredCommandBuffer& command_buffer) const;
  void WriteDump();

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  bool initialized_ = false;
  bool failed_ = false;
  std::filesystem::path output_dir_;

  std::array<VkImage, kRingSize> frames_{};
  std::array<VkDeviceMemory, kRingSize> frames_memory_{};
  VkImage thumb_ = VK_NULL_HANDLE;
  VkDeviceMemory thumb_memory_ = VK_NULL_HANDLE;
  VkBuffer thumb_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory thumb_buffer_memory_ = VK_NULL_HANDLE;
  const uint8_t* thumb_mapping_ = nullptr;
  VkBuffer dump_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory dump_buffer_memory_ = VK_NULL_HANDLE;
  const uint8_t* dump_mapping_ = nullptr;
  bool images_initialized_ = false;

  std::array<Slot, kRingSize> slots_{};
  // Luma thumbnails of the last three consecutive scored frames.
  std::array<std::vector<uint8_t>, 3> history_;
  uint64_t history_last_frame_ = UINT64_MAX;
  uint32_t history_count_ = 0;

  uint64_t dump_submission_ = UINT64_MAX;
  uint64_t dump_frame_ = 0;
  float dump_scores_[3] = {};
  uint64_t last_dump_frame_ = 0;
  uint32_t dumps_ = 0;
  uint64_t scored_frames_ = 0;
  uint64_t candidates_ = 0;
  uint64_t hud1_candidates_ = 0;
  uint64_t hud1_frame_ = UINT64_MAX;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_GLITCH_RECORDER_H_
