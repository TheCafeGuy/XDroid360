/**
 * XDroid360 GSR2-1: Snapdragon Game Super Resolution 2 (2-pass compute) in the
 * swap, fed by Ace Combat 6 data: the world pass depth, the view-projection
 * matrices the game keeps for this and the previous frame (c116-c123), the
 * horizontal FOV (c104.x) and a Halton jitter added to the world draws.
 * Other titles never run it.
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_GSR2_H_
#define XENIA_GPU_VULKAN_VULKAN_GSR2_H_

#include <array>
#include <cstdint>

#include "xenia/gpu/register_file.h"
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

class VulkanGsr2 {
 public:
  ~VulkanGsr2() { Shutdown(); }

  // The title gate and the live switch.
  static bool IsOn(uint32_t title_id);
  // Output size for the guest output: gsr2_output_height at 16:9.
  static void GetOutputSize(uint32_t& width, uint32_t& height);

  // Called before the render target update of every draw. Returns true when
  // the world pass just ended and its depth must be copied now.
  bool OnDraw(const RegisterFile& regs);
  // NDC offset to add to this draw's ndc_offset (zero outside the world pass).
  void GetNdcJitter(float& x, float& y) const;
  // Whether this frame has a world pass with depth, matrices and FOV.
  bool FrameReady() const { return depth_copied_; }

  bool EnsureInitialized(const ui::vulkan::VulkanDevice* vulkan_device,
                         uint32_t out_width, uint32_t out_height);
  void Shutdown();
  bool initialized() const { return initialized_; }

  // The caller has ended the render pass and submitted its barriers.
  // depth_image has depth+stencil aspects in DEPTH_STENCIL_ATTACHMENT_OPTIMAL
  // and is left in it.
  void CopyDepth(DeferredCommandBuffer& cb, VkImage depth_image,
                 VkImageView depth_view, VkSampleCountFlagBits samples);
  // color_view is in SHADER_READ_ONLY_OPTIMAL, out_view in GENERAL.
  void Run(DeferredCommandBuffer& cb, VkImageView color_view,
           VkImageView out_view);
  // Ends the frame after the swap (with or without Run).
  void EndFrame(bool ran);

 private:
  static constexpr uint32_t kRing = 4;
  static constexpr uint32_t kRenderWidth = 1280;
  static constexpr uint32_t kRenderHeight = 720;
  static constexpr uint32_t kBindings = 12;

  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  bool CreateImage(Image& out, VkFormat format, uint32_t w, uint32_t h);
  void Barrier(DeferredCommandBuffer& cb);

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  bool initialized_ = false;
  bool failed_ = false;
  uint32_t out_width_ = 0, out_height_ = 0;

  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kRing> sets_{};
  VkPipeline depth_pipeline_ = VK_NULL_HANDLE;
  VkPipeline convert_pipeline_ = VK_NULL_HANDLE;
  VkPipeline upscale_pipeline_ = VK_NULL_HANDLE;
  VkSampler linear_sampler_ = VK_NULL_HANDLE;
  VkSampler nearest_sampler_ = VK_NULL_HANDLE;
  VkBuffer ubo_ = VK_NULL_HANDLE;
  VkDeviceMemory ubo_memory_ = VK_NULL_HANDLE;
  uint32_t ubo_memory_type_ = 0;
  VkDeviceSize ubo_memory_size_ = 0;
  uint8_t* ubo_mapping_ = nullptr;
  VkDeviceSize ubo_stride_ = 0;
  Image depth_, mdca_, ycocg_;
  std::array<Image, 2> history_;
  bool layouts_ready_ = false;

  // Per-frame capture.
  uint32_t world_draws_ = 0;
  bool draw_is_world_ = false;
  bool captured_ = false;
  bool depth_tried_ = false;
  bool depth_copied_ = false;
  double view_proj_[2][16] = {};  // [0] this frame, [1] previous (columns).
  float fov_ = 0.0f;
  // Across frames.
  bool prev_ran_ = false;
  bool ran_once_ = false;
  uint32_t frame_counter_ = 0;
  uint32_t slot_ = 0;
  uint32_t history_index_ = 0;
  uint32_t log_frames_ = 0;
  uint32_t log_world_draws_ = 0;
  uint32_t log_resets_ = 0;
  float jitter_px_[2] = {};
  bool reset_next_ = true;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_GSR2_H_
