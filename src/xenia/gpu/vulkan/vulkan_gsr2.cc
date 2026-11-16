/**
 * XDroid360 GSR2-1: Snapdragon GSR 2 in the swap (see vulkan_gsr2.h). The
 * shaders are ported from sgsr/v2 (BSD-3-Clause, docs/licenses).
 */

#include "xenia/gpu/vulkan/vulkan_gsr2.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DEFINE_bool(gsr2, false,
            "GSR2-1: Snapdragon Game Super Resolution 2 temporal upscale of "
            "the Ace Combat 6 swap. Live.",
            "XDroid360");
DEFINE_bool(gsr2_any_title, false,
            "GSR2: skip the Ace Combat 6 title gate (trace replay bench has no "
            "title). Live.",
            "XDroid360");
DEFINE_int32(gsr2_output_height, 1080,
             "GSR2 output height (720, 1080 or 1440; 16:9). Live.",
             "XDroid360");
DEFINE_bool(gsr2_jitter, true,
            "GSR2: Halton sub-pixel jitter on world draws. Live.", "XDroid360");
DEFINE_int32(gsr2_jitter_ysign, 1,
             "GSR2: sign (1 or -1) of the jitter added to the NDC y. Live.",
             "XDroid360");
DEFINE_bool(gsr2_hud_mask, true,
            "GSR2: keep screen-static overlay (HUD) pixels out of the motion "
            "reprojection. Live.",
            "XDroid360");
DEFINE_bool(gsr2_vp_swap, false,
            "GSR2: use c120-c123 as the current and c116-c119 as the previous "
            "view-projection. Live.",
            "XDroid360");

namespace xe {
namespace gpu {
namespace vulkan {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/sgsr2_convert_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/sgsr2_depth_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/sgsr2_upscale_cs.h"
}  // namespace shaders

namespace {

constexpr uint32_t kAc6TitleId = 0x4E4D07D1;
// AC6 world pass: 2x MSAA 1280x720 color + D24FS8 depth at EDRAM tile 0x2D0.
constexpr uint32_t kWorldSurfaceInfo = 0x0A010280;
constexpr uint32_t kWorldDepthInfo = 0x000102D0;
constexpr uint32_t kCaptureDraw = 100;
constexpr uint32_t kHaltonPhases = 8;
constexpr uint32_t kLogEvery = 300;

struct Ubo {  // std140 Params of the sgsr2 shaders.
  uint32_t render_size[2];
  uint32_t display_size[2];
  float render_size_rcp[2];
  float display_size_rcp[2];
  float jitter[2];
  float pad[2];
  float clip_to_prev[16];
  float pre_exposure;
  float fov;
  float near_plane;
  float min_lerp;
  uint32_t same_camera;
  uint32_t reset;
  uint32_t hud_mask;
};

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (; index; index /= base) {
    f /= float(base);
    r += f * float(index % base);
  }
  return r;
}

// Column-major 4x4 product out = a * b.
void Mul(const double* a, const double* b, double* out) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      double s = 0.0;
      for (int k = 0; k < 4; ++k) {
        s += a[k * 4 + r] * b[c * 4 + k];
      }
      out[c * 4 + r] = s;
    }
  }
}

// Gauss-Jordan inverse of a column-major 4x4 (false if singular).
bool Invert(const double* m, double* inv) {
  double a[4][4], b[4][4] = {};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[c * 4 + r];
    }
    b[r][r] = 1.0;
  }
  for (int col = 0; col < 4; ++col) {
    int piv = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) {
        piv = r;
      }
    }
    if (std::fabs(a[piv][col]) < 1e-300) {
      return false;
    }
    for (int c = 0; c < 4; ++c) {
      std::swap(a[col][c], a[piv][c]);
      std::swap(b[col][c], b[piv][c]);
    }
    const double d = 1.0 / a[col][col];
    for (int c = 0; c < 4; ++c) {
      a[col][c] *= d;
      b[col][c] *= d;
    }
    for (int r = 0; r < 4; ++r) {
      if (r == col) {
        continue;
      }
      const double f = a[r][col];
      for (int c = 0; c < 4; ++c) {
        a[r][c] -= f * a[col][c];
        b[r][c] -= f * b[col][c];
      }
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      inv[c * 4 + r] = b[r][c];
    }
  }
  return true;
}

}  // namespace

bool VulkanGsr2::IsOn(uint32_t title_id) {
  return cvars::gsr2 && (cvars::gsr2_any_title || title_id == kAc6TitleId);
}

void VulkanGsr2::GetOutputSize(uint32_t& width, uint32_t& height) {
  height = uint32_t(std::clamp(cvars::gsr2_output_height, 720, 1440));
  width = height * 16 / 9;
}

bool VulkanGsr2::OnDraw(const RegisterFile& regs) {
  draw_is_world_ = regs[XE_GPU_REG_RB_SURFACE_INFO] == kWorldSurfaceInfo &&
                   regs[XE_GPU_REG_RB_DEPTH_INFO] == kWorldDepthInfo;
  if (draw_is_world_) {
    if (++world_draws_ == kCaptureDraw) {
      // c116-c119 view-projection of this frame, c120-c123 the previous
      // frame's (columns), c104.x = tan(hfov / 2).
      const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_000_X;
      for (uint32_t i = 0; i < 8; ++i) {
        for (uint32_t k = 0; k < 4; ++k) {
          const uint32_t bits = regs[base + 4 * (116 + i) + k];
          float v;
          std::memcpy(&v, &bits, sizeof(v));
          view_proj_[i / 4][(i % 4) * 4 + k] = v;
        }
      }
      const uint32_t bits = regs[base + 4 * 104];
      std::memcpy(&fov_, &bits, sizeof(fov_));
      captured_ = true;
    }
    return false;
  }
  if (captured_ && !depth_tried_) {
    depth_tried_ = true;
    return true;
  }
  return false;
}

void VulkanGsr2::GetNdcJitter(float& x, float& y) const {
  x = y = 0.0f;
  if (draw_is_world_ && cvars::gsr2_jitter) {
    x = 2.0f * jitter_px_[0] / float(kRenderWidth);
    y = 2.0f * jitter_px_[1] / float(kRenderHeight) *
        (cvars::gsr2_jitter_ysign < 0 ? -1.0f : 1.0f);
  }
}

bool VulkanGsr2::CreateImage(Image& out, VkFormat format, uint32_t w,
                             uint32_t h) {
  VkImageCreateInfo ci = {};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device_, ci, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          out.image, out.memory)) {
    return false;
  }
  VkImageViewCreateInfo vi = {};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  return vulkan_device_->functions().vkCreateImageView(
             vulkan_device_->device(), &vi, nullptr, &out.view) == VK_SUCCESS;
}

bool VulkanGsr2::EnsureInitialized(
    const ui::vulkan::VulkanDevice* vulkan_device, uint32_t out_width,
    uint32_t out_height) {
  if (out_width == out_width_ && out_height == out_height_) {
    if (initialized_) {
      return true;
    }
    if (failed_) {
      return false;
    }
  }
  Shutdown();
  failed_ = true;
  vulkan_device_ = vulkan_device;
  out_width_ = out_width;
  out_height_ = out_height;
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  static const VkDescriptorType kTypes[kBindings] = {
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  VkDescriptorSetLayoutBinding bindings[kBindings] = {};
  for (uint32_t i = 0; i < kBindings; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = kTypes[i];
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo layout_ci = {};
  layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_ci.bindingCount = kBindings;
  layout_ci.pBindings = bindings;
  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kRing},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kRing * 6},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kRing * 5}};
  VkDescriptorPoolCreateInfo pool_ci = {};
  pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_ci.maxSets = kRing;
  pool_ci.poolSizeCount = 3;
  pool_ci.pPoolSizes = pool_sizes;
  VkSamplerCreateInfo sampler_ci = {};
  sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_ci.addressModeU = sampler_ci.addressModeV = sampler_ci.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_ci.magFilter = sampler_ci.minFilter = VK_FILTER_LINEAR;

  int stage = 0;
  bool ok = dfn.vkCreateDescriptorSetLayout(device, &layout_ci, nullptr,
                                            &set_layout_) == VK_SUCCESS;
  stage = 1;
  if (ok) {
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl_ci = {};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &set_layout_;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push;
    ok = dfn.vkCreatePipelineLayout(device, &pl_ci, nullptr,
                                    &pipeline_layout_) == VK_SUCCESS &&
         dfn.vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool_) ==
             VK_SUCCESS &&
         dfn.vkCreateSampler(device, &sampler_ci, nullptr, &linear_sampler_) ==
             VK_SUCCESS;
    sampler_ci.magFilter = sampler_ci.minFilter = VK_FILTER_NEAREST;
    ok = ok && dfn.vkCreateSampler(device, &sampler_ci, nullptr,
                                   &nearest_sampler_) == VK_SUCCESS;
  }
  stage = 2;
  if (ok) {
    std::array<VkDescriptorSetLayout, kRing> layouts;
    layouts.fill(set_layout_);
    VkDescriptorSetAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kRing;
    ai.pSetLayouts = layouts.data();
    ok = dfn.vkAllocateDescriptorSets(device, &ai, sets_.data()) == VK_SUCCESS;
  }
  stage = 3;
  if (ok) {
    ubo_stride_ = std::max<VkDeviceSize>(
        256, vulkan_device->properties().minUniformBufferOffsetAlignment);
    void* mapping = nullptr;
    ok = ui::vulkan::util::CreateDedicatedAllocationBuffer(
             vulkan_device, ubo_stride_ * kRing,
             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
             ui::vulkan::util::MemoryPurpose::kUpload, ubo_, ubo_memory_,
             &ubo_memory_type_, &ubo_memory_size_) &&
         dfn.vkMapMemory(device, ubo_memory_, 0, VK_WHOLE_SIZE, 0, &mapping) ==
             VK_SUCCESS;
    ubo_mapping_ = static_cast<uint8_t*>(mapping);
  }
  stage = 4;
  if (ok) {
    ok = CreateImage(depth_, VK_FORMAT_R32_SFLOAT, kRenderWidth,
                     kRenderHeight) &&
         CreateImage(mdca_, VK_FORMAT_R16G16B16A16_SFLOAT, kRenderWidth,
                     kRenderHeight) &&
         CreateImage(ycocg_, VK_FORMAT_R32_UINT, kRenderWidth, kRenderHeight) &&
         CreateImage(history_[0], VK_FORMAT_R16G16B16A16_SFLOAT, out_width,
                     out_height) &&
         CreateImage(history_[1], VK_FORMAT_R16G16B16A16_SFLOAT, out_width,
                     out_height);
  }
  stage = 5;
  if (ok) {
    struct {
      const uint32_t* code;
      size_t size;
      VkPipeline* pipeline;
    } shaders[] = {
        {shaders::sgsr2_depth_cs, sizeof(shaders::sgsr2_depth_cs),
         &depth_pipeline_},
        {shaders::sgsr2_convert_cs, sizeof(shaders::sgsr2_convert_cs),
         &convert_pipeline_},
        {shaders::sgsr2_upscale_cs, sizeof(shaders::sgsr2_upscale_cs),
         &upscale_pipeline_}};
    for (auto& s : shaders) {
      VkShaderModule module =
          ui::vulkan::util::CreateShaderModule(vulkan_device, s.code, s.size);
      if (module == VK_NULL_HANDLE) {
        ok = false;
        stage = 10 + int(&s - shaders);
        break;
      }
      *s.pipeline = ui::vulkan::util::CreateComputePipeline(
          vulkan_device, pipeline_layout_, module);
      dfn.vkDestroyShaderModule(device, module, nullptr);
      if (*s.pipeline == VK_NULL_HANDLE) {
        ok = false;
        stage = 20 + int(&s - shaders);
        break;
      }
    }
  }
  if (!ok) {
    XELOGE("XDROID-GSR2 init failed ({}x{}) stage={}", out_width, out_height,
           stage);
    Shutdown();
    return false;
  }
  failed_ = false;
  initialized_ = true;
  layouts_ready_ = false;
  reset_next_ = true;
  XELOGI("XDROID-GSR2 on: {}x{} -> {}x{}", kRenderWidth, kRenderHeight,
         out_width, out_height);
  return true;
}

void VulkanGsr2::Shutdown() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  using ui::vulkan::util::DestroyAndNullHandle;
  for (VkPipeline* p :
       {&depth_pipeline_, &convert_pipeline_, &upscale_pipeline_}) {
    DestroyAndNullHandle(dfn.vkDestroyPipeline, device, *p);
  }
  for (Image* img : {&depth_, &mdca_, &ycocg_, &history_[0], &history_[1]}) {
    DestroyAndNullHandle(dfn.vkDestroyImageView, device, img->view);
    DestroyAndNullHandle(dfn.vkDestroyImage, device, img->image);
    DestroyAndNullHandle(dfn.vkFreeMemory, device, img->memory);
  }
  DestroyAndNullHandle(dfn.vkDestroyBuffer, device, ubo_);
  DestroyAndNullHandle(dfn.vkFreeMemory, device, ubo_memory_);
  ubo_mapping_ = nullptr;
  DestroyAndNullHandle(dfn.vkDestroySampler, device, linear_sampler_);
  DestroyAndNullHandle(dfn.vkDestroySampler, device, nearest_sampler_);
  DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, pool_);
  DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline_layout_);
  DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, set_layout_);
  initialized_ = false;
  layouts_ready_ = false;
}

void VulkanGsr2::Barrier(DeferredCommandBuffer& cb) {
  VkMemoryBarrier mb = {
      VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  cb.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void VulkanGsr2::CopyDepth(DeferredCommandBuffer& cb, VkImage depth_image,
                           VkImageView depth_view,
                           VkSampleCountFlagBits samples) {
  if (!initialized_ || samples == VK_SAMPLE_COUNT_1_BIT) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  slot_ = frame_counter_++ % kRing;

  if (!layouts_ready_) {
    const Image* imgs[5] = {&depth_, &mdca_, &ycocg_, &history_[0],
                            &history_[1]};
    VkImageMemoryBarrier b[5] = {};
    for (int i = 0; i < 5; ++i) {
      b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex =
          VK_QUEUE_FAMILY_IGNORED;
      b[i].image = imgs[i]->image;
      b[i].subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
    }
    cb.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                            nullptr, 5, b);
    // Histories start black: the reset frame multiplies them by zero.
    VkClearColorValue zero = {};
    VkImageSubresourceRange range =
        ui::vulkan::util::InitializeSubresourceRange();
    for (int i = 3; i < 5; ++i) {
      cb.CmdVkClearColorImage(imgs[i]->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                              &range);
    }
    layouts_ready_ = true;
  }

  // Descriptors 10 (MS depth) and 11 (R32F destination) of this slot.
  VkDescriptorImageInfo src = {
      nearest_sampler_, depth_view,
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo dst = {VK_NULL_HANDLE, depth_.view,
                               VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w[2] = {};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = sets_[slot_];
    w[i].dstBinding = 10 + i;
    w[i].descriptorCount = 1;
  }
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w[0].pImageInfo = &src;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[1].pImageInfo = &dst;
  dfn.vkUpdateDescriptorSets(device, 2, w, 0, nullptr);

  // Attachment -> read-only for the sampled copy; the memory barrier also
  // orders the previous frame's reads of the destination before our write.
  VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT};
  VkImageMemoryBarrier ib = {};
  ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  ib.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  ib.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  ib.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ib.image = depth_image;
  ib.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  cb.CmdVkPipelineBarrier(
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 1, &ib);

  cb.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, depth_pipeline_);
  cb.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_,
                             0, 1, &sets_[slot_], 0, nullptr);
  const uint32_t size[2] = {kRenderWidth, kRenderHeight};
  cb.CmdVkPushConstants(pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                        sizeof(size), size);
  cb.CmdVkDispatch((kRenderWidth + 7) / 8, (kRenderHeight + 7) / 8, 1);

  ib.srcAccessMask = 0;
  ib.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  ib.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  ib.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  cb.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &ib);
  depth_copied_ = true;
}

void VulkanGsr2::Run(DeferredCommandBuffer& cb, VkImageView color_view,
                     VkImageView out_view) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  const uint32_t prev = history_index_ ^ 1;

  // Camera: clip_to_prev = VP_prev * inverse(VP_cur).
  const int cur_i = cvars::gsr2_vp_swap ? 1 : 0;
  double inv[16], m[16];
  bool reset = reset_next_ || !prev_ran_;
  float clip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (Invert(view_proj_[cur_i], inv)) {
    Mul(view_proj_[cur_i ^ 1], inv, m);
    for (int i = 0; i < 16; ++i) {
      clip[i] = float(m[i]);
    }
  } else {
    reset = true;
  }
  if (reset) {
    ++log_resets_;
  }

  Ubo ubo = {};
  ubo.render_size[0] = kRenderWidth;
  ubo.render_size[1] = kRenderHeight;
  ubo.display_size[0] = out_width_;
  ubo.display_size[1] = out_height_;
  ubo.render_size_rcp[0] = 1.0f / float(kRenderWidth);
  ubo.render_size_rcp[1] = 1.0f / float(kRenderHeight);
  ubo.display_size_rcp[0] = 1.0f / float(out_width_);
  ubo.display_size_rcp[1] = 1.0f / float(out_height_);
  ubo.jitter[0] = cvars::gsr2_jitter ? jitter_px_[0] : 0.0f;
  ubo.jitter[1] = cvars::gsr2_jitter ? jitter_px_[1] : 0.0f;
  std::memcpy(ubo.clip_to_prev, clip, sizeof(clip));
  ubo.pre_exposure = 1.0f;
  ubo.fov = fov_;
  ubo.near_plane = 0.1f;
  ubo.min_lerp = 0.3f;
  ubo.same_camera = 0;
  ubo.reset = reset ? 1 : 0;
  ubo.hud_mask = cvars::gsr2_hud_mask ? 1 : 0;
  const VkDeviceSize ubo_offset = ubo_stride_ * slot_;
  std::memcpy(ubo_mapping_ + ubo_offset, &ubo, sizeof(ubo));
  ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, ubo_memory_,
                                           ubo_memory_type_, ubo_offset,
                                           ubo_memory_size_, sizeof(ubo));

  VkDescriptorBufferInfo ubo_info = {ubo_, ubo_offset, sizeof(Ubo)};
  const VkImageLayout g = VK_IMAGE_LAYOUT_GENERAL;
  // Bindings 1..9.
  VkDescriptorImageInfo infos[9] = {
      {linear_sampler_, color_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {nearest_sampler_, depth_.view, g},
      {VK_NULL_HANDLE, mdca_.view, g},
      {VK_NULL_HANDLE, ycocg_.view, g},
      {linear_sampler_, mdca_.view, g},
      {nearest_sampler_, ycocg_.view, g},
      {linear_sampler_, history_[prev].view, g},
      {VK_NULL_HANDLE, out_view, g},
      {VK_NULL_HANDLE, history_[history_index_].view, g}};
  static const VkDescriptorType kRunTypes[10] = {
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  VkWriteDescriptorSet w[10] = {};
  for (uint32_t i = 0; i < 10; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = sets_[slot_];
    w[i].dstBinding = i;
    w[i].descriptorCount = 1;
    w[i].descriptorType = kRunTypes[i];
    if (i == 0) {
      w[i].pBufferInfo = &ubo_info;
    } else {
      w[i].pImageInfo = &infos[i - 1];
    }
  }
  dfn.vkUpdateDescriptorSets(device, 10, w, 0, nullptr);

  Barrier(cb);  // depth copy (and first-use clears) -> convert
  cb.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_,
                             0, 1, &sets_[slot_], 0, nullptr);
  cb.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, convert_pipeline_);
  cb.CmdVkDispatch((kRenderWidth + 7) / 8, (kRenderHeight + 7) / 8, 1);
  Barrier(cb);
  cb.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, upscale_pipeline_);
  cb.CmdVkDispatch((out_width_ + 7) / 8, (out_height_ + 7) / 8, 1);
  history_index_ = prev;
  reset_next_ = false;
}

void VulkanGsr2::EndFrame(bool ran) {
  if (++log_frames_ >= kLogEvery) {
    XELOGI("XDROID-GSR2 world={} draws={} depth={} ran={} resets={} fov={:.3f}",
           log_world_draws_ ? 1 : 0, log_world_draws_, depth_copied_ ? 1 : 0,
           ran ? 1 : 0, log_resets_, fov_);
    log_frames_ = 0;
    log_resets_ = 0;
  }
  log_world_draws_ = world_draws_;
  prev_ran_ = ran;
  world_draws_ = 0;
  captured_ = depth_tried_ = depth_copied_ = false;
  const uint32_t idx = frame_counter_ % kHaltonPhases + 1;
  jitter_px_[0] = Halton(idx, 2) - 0.5f;
  jitter_px_[1] = Halton(idx, 3) - 0.5f;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
