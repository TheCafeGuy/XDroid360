/**
 * XDroid360 forensic tool (IS-2). See vulkan_glitch_recorder.h.
 */

#include "xenia/gpu/vulkan/vulkan_glitch_recorder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/xdroid_gpu_hooks.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace gpu {
namespace vulkan {

bool VulkanGlitchRecorder::EnsureInitialized(
    const ui::vulkan::VulkanDevice* vulkan_device,
    const std::filesystem::path& output_dir) {
  if (initialized_) {
    return true;
  }
  if (failed_) {
    return false;
  }
  failed_ = true;
  vulkan_device_ = vulkan_device;
  output_dir_ = output_dir;
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkImageCreateInfo image_create_info = {};
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_create_info.extent = {kFrameWidth, kFrameHeight, 1};
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  for (uint32_t i = 0; i < kRingSize; ++i) {
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device, image_create_info,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, frames_[i],
            frames_memory_[i])) {
      XELOGE("XDroid360: glitch recorder: frame image creation failed");
      Shutdown();
      return false;
    }
  }
  image_create_info.extent = {kThumbWidth, kThumbHeight, 1};
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, thumb_,
          thumb_memory_)) {
    XELOGE("XDroid360: glitch recorder: thumbnail image creation failed");
    Shutdown();
    return false;
  }
  void* mapping;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, VkDeviceSize(kThumbBytes) * kRingSize,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, thumb_buffer_,
          thumb_buffer_memory_) ||
      dfn.vkMapMemory(device, thumb_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                      &mapping) != VK_SUCCESS) {
    XELOGE("XDroid360: glitch recorder: thumbnail buffer creation failed");
    Shutdown();
    return false;
  }
  thumb_mapping_ = static_cast<const uint8_t*>(mapping);
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, VkDeviceSize(kFrameBytes) * kDumpFrames,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, dump_buffer_,
          dump_buffer_memory_) ||
      dfn.vkMapMemory(device, dump_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                      &mapping) != VK_SUCCESS) {
    XELOGE("XDroid360: glitch recorder: dump buffer creation failed");
    Shutdown();
    return false;
  }
  dump_mapping_ = static_cast<const uint8_t*>(mapping);
  xe::filesystem::CreateFolder(output_dir_);
  for (std::vector<uint8_t>& luma : history_) {
    luma.assign(kThumbWidth * kThumbHeight, 0);
  }
  failed_ = false;
  initialized_ = true;
  XELOGI("XDroid360: glitch recorder on, dumps go to {}",
         xe::path_to_utf8(output_dir_));
  return true;
}

void VulkanGlitchRecorder::Shutdown() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         dump_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         dump_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         thumb_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         thumb_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, thumb_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         thumb_memory_);
  for (uint32_t i = 0; i < kRingSize; ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device,
                                           frames_[i]);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           frames_memory_[i]);
  }
  thumb_mapping_ = nullptr;
  dump_mapping_ = nullptr;
  images_initialized_ = false;
  initialized_ = false;
  vulkan_device_ = nullptr;
}

void VulkanGlitchRecorder::TransferBarrier(
    DeferredCommandBuffer& command_buffer) const {
  // The ring, the thumbnail and the buffers are only touched by transfers:
  // one global barrier orders every read and write of the previous step.
  VkMemoryBarrier barrier;
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.srcAccessMask =
      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask =
      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  command_buffer.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                      &barrier, 0, nullptr, 0, nullptr);
}

void VulkanGlitchRecorder::Poll(DeferredCommandBuffer& command_buffer,
                                uint64_t completed_submission,
                                uint64_t current_submission, uint64_t frame) {
  if (!initialized_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (dump_submission_ <= completed_submission) {
    WriteDump();
    dump_submission_ = UINT64_MAX;
  }

  // Finished thumbnails, oldest frame first.
  std::array<uint32_t, kRingSize> ready;
  uint32_t ready_count = 0;
  for (uint32_t i = 0; i < kRingSize; ++i) {
    const Slot& slot = slots_[i];
    if (slot.frame != UINT64_MAX && !slot.scored &&
        slot.submission <= completed_submission) {
      ready[ready_count++] = i;
    }
  }
  if (!ready_count) {
    return;
  }
  std::sort(ready.begin(), ready.begin() + ready_count,
            [this](uint32_t a, uint32_t b) {
              return slots_[a].frame < slots_[b].frame;
            });
  VkMappedMemoryRange range;
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.pNext = nullptr;
  range.memory = thumb_buffer_memory_;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);

  auto diff = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    uint32_t sum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
      sum += uint32_t(std::abs(int(a[i]) - int(b[i])));
    }
    return float(sum) / float(a.size());
  };
  // The same over one of 8x6 blocks of the thumbnail, so a glitch the size
  // of a HUD element (the radar) stands out of a frame that is otherwise
  // unchanged.
  constexpr uint32_t kBlockWidth = kThumbWidth / 8;
  constexpr uint32_t kBlockHeight = kThumbHeight / 6;
  auto block_diff = [](const std::vector<uint8_t>& a,
                       const std::vector<uint8_t>& b, uint32_t block_x,
                       uint32_t block_y) {
    uint32_t sum = 0;
    for (uint32_t y = 0; y < kBlockHeight; ++y) {
      const size_t row = size_t(block_y * kBlockHeight + y) * kThumbWidth +
                         block_x * kBlockWidth;
      for (uint32_t x = 0; x < kBlockWidth; ++x) {
        sum += uint32_t(std::abs(int(a[row + x]) - int(b[row + x])));
      }
    }
    return float(sum) / float(kBlockWidth * kBlockHeight);
  };

  for (uint32_t r = 0; r < ready_count; ++r) {
    Slot& slot = slots_[ready[r]];
    slot.scored = true;
    if (history_last_frame_ == UINT64_MAX ||
        slot.frame != history_last_frame_ + 1) {
      history_count_ = 0;
    }
    history_last_frame_ = slot.frame;
    std::swap(history_[0], history_[1]);
    std::swap(history_[1], history_[2]);
    const uint8_t* rgba = thumb_mapping_ + size_t(kThumbBytes) * ready[r];
    std::vector<uint8_t>& luma = history_[2];
    for (uint32_t i = 0; i < kThumbWidth * kThumbHeight; ++i) {
      luma[i] = uint8_t((uint32_t(rgba[i * 4]) + 2 * uint32_t(rgba[i * 4 + 1]) +
                         uint32_t(rgba[i * 4 + 2])) >>
                        2);
    }
    history_count_ = std::min(history_count_ + 1, uint32_t(3));
    if (history_count_ < 3) {
      continue;
    }
    // Frame n = slot.frame - 1 against its neighbours, over the whole frame
    // and per block (the block that stands out most).
    float a = diff(history_[0], history_[1]);
    float b = diff(history_[1], history_[2]);
    float c = diff(history_[0], history_[2]);
    bool candidate = std::min(a, b) >= 4.0f && std::min(a, b) > 2.5f * c + 1.0f;
    int32_t block = -1;
    if (!candidate) {
      float best = 0.0f;
      for (uint32_t block_y = 0; block_y < 6; ++block_y) {
        for (uint32_t block_x = 0; block_x < 8; ++block_x) {
          const float block_a =
              block_diff(history_[0], history_[1], block_x, block_y);
          const float block_b =
              block_diff(history_[1], history_[2], block_x, block_y);
          const float block_changed = std::min(block_a, block_b);
          if (block_changed < 16.0f) {
            continue;
          }
          const float block_c =
              block_diff(history_[0], history_[2], block_x, block_y);
          const float score = block_changed - 2.5f * block_c - 2.0f;
          if (score > best) {
            best = score;
            block = int32_t(block_y * 8 + block_x);
            a = block_a;
            b = block_b;
            c = block_c;
          }
        }
      }
      candidate = block >= 0;
    }
    // HUD-1 (the radar quad drawn with the HUD atlas or a flat colour): a
    // radar block (x 0-1, y 3-5) is a strong one-frame outlier while its
    // neighbours agree. The HUD-1 frames the user marked in hud1-c and the
    // one found by eye in hud1-w (f7266) triggered there; camera motion and
    // explosions, the bulk of the plain candidates, rarely do.
    bool hud1 = false;
    for (uint32_t block_y = 3; block_y < 6; ++block_y) {
      for (uint32_t block_x = 0; block_x < 2; ++block_x) {
        const float block_a =
            block_diff(history_[0], history_[1], block_x, block_y);
        const float block_b =
            block_diff(history_[1], history_[2], block_x, block_y);
        const float block_c =
            block_diff(history_[0], history_[2], block_x, block_y);
        if (std::min(block_a, block_b) >= 24.0f && block_c < 8.0f &&
            (!hud1 || std::min(block_a, block_b) > std::min(a, b))) {
          hud1 = true;
          candidate = true;
          block = int32_t(block_y * 8 + block_x);
          a = block_a;
          b = block_b;
          c = block_c;
        }
      }
    }
    ++scored_frames_;
    if (!(scored_frames_ % 1800)) {
      XELOGI(
          "XDroid360: glitch recorder: {} frames scored, {} candidates, {} "
          "HUD-1 (radar)",
          scored_frames_, candidates_, hud1_candidates_);
    }
    if (!candidate) {
      continue;
    }
    ++candidates_;
    const uint64_t n = slot.frame - 1;
    if (hud1) {
      ++hud1_candidates_;
      hud1_frame_ = n;
      XELOGW("XDROID-HUD1: frame {} radar block {},{} (d(n-1,n)={:.1f} "
             "d(n,n+1)={:.1f} d(n-1,n+1)={:.1f}), total {}",
             n, block % 8, block / 8, a, b, c, hud1_candidates_);
    }
    // Dumps n-2..n+1: n-2 tells whether frame n repeats n-1 or older content.
    bool frames_in_ring = frame - (n - 2) < kRingSize - 1;
    for (uint64_t f = n - 2; f <= n + 1; ++f) {
      frames_in_ring &= slots_[f % kRingSize].frame == f;
    }
    const bool dump = frames_in_ring && dump_submission_ == UINT64_MAX &&
                      dumps_ < kMaxDumps &&
                      (hud1 || !last_dump_frame_ || n >= last_dump_frame_ + 30);
    XELOGW(
        "XDROID-GLITCH: frame {} differs from both neighbours in {} "
        "(d(n-1,n)={:.1f} d(n,n+1)={:.1f} d(n-1,n+1)={:.1f}){}",
        n,
        block < 0 ? std::string("the whole frame")
                  : fmt::format("block {},{} of 8x6", block % 8, block / 8),
        a, b, c, dump ? ", dumping" : "");
    // HUD-1 tracer: |frame| counts our captures, the log prefix counts CP
    // swaps; hand over n on the log's scale.
    if (auto on_glitch =
            GetXdroidGpuHooks().on_glitch.load(std::memory_order_acquire)) {
      on_glitch(uint32_t(n + (logging::GetFrameNumber() - frame)));
    }
    if (!dump) {
      continue;
    }
    TransferBarrier(command_buffer);
    for (uint32_t k = 0; k < kDumpFrames; ++k) {
      VkBufferImageCopy copy = {};
      copy.bufferOffset = VkDeviceSize(kFrameBytes) * k;
      copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.imageSubresource.layerCount = 1;
      copy.imageExtent = {kFrameWidth, kFrameHeight, 1};
      command_buffer.CmdVkCopyImageToBuffer(frames_[(n - 2 + k) % kRingSize],
                                            VK_IMAGE_LAYOUT_GENERAL,
                                            dump_buffer_, 1, &copy);
    }
    dump_submission_ = current_submission;
    dump_frame_ = n;
    dump_scores_[0] = a;
    dump_scores_[1] = b;
    dump_scores_[2] = c;
    last_dump_frame_ = n;
    ++dumps_;
  }
}

void VulkanGlitchRecorder::WriteDump() {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  VkMappedMemoryRange range;
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.pNext = nullptr;
  range.memory = dump_buffer_memory_;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(vulkan_device_->device(), 1, &range);
  // RGB, frames n-2, n-1, n, n+1 side by side, written off the GPU thread.
  std::vector<uint8_t> rgb(size_t(kFrameWidth) * kDumpFrames * kFrameHeight *
                           3);
  for (uint32_t k = 0; k < kDumpFrames; ++k) {
    const uint8_t* src = dump_mapping_ + size_t(kFrameBytes) * k;
    for (uint32_t y = 0; y < kFrameHeight; ++y) {
      uint8_t* dst = rgb.data() + (size_t(y) * kFrameWidth * kDumpFrames +
                                   size_t(k) * kFrameWidth) *
                                      3;
      for (uint32_t x = 0; x < kFrameWidth; ++x) {
        const uint8_t* p = src + (size_t(y) * kFrameWidth + x) * 4;
        dst[x * 3] = p[0];
        dst[x * 3 + 1] = p[1];
        dst[x * 3 + 2] = p[2];
      }
    }
  }
  char name[64];
  std::snprintf(name, sizeof(name), "f%07llu.ppm",
                static_cast<unsigned long long>(dump_frame_));
  std::filesystem::path path = output_dir_ / name;
  XELOGI("XDROID-GLITCH: frame {} saved as {} (n-2, n-1, n, n+1)", dump_frame_,
         xe::path_to_utf8(path));
  std::thread([path, rgb = std::move(rgb)] {
    FILE* file = xe::filesystem::OpenFile(path, "wb");
    if (!file) {
      return;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", kFrameWidth * kDumpFrames,
                 kFrameHeight);
    std::fwrite(rgb.data(), 1, rgb.size(), file);
    std::fclose(file);
  }).detach();
}

void VulkanGlitchRecorder::Capture(DeferredCommandBuffer& command_buffer,
                                   VkImage guest_output, uint32_t width,
                                   uint32_t height,
                                   uint64_t current_submission,
                                   uint64_t frame) {
  if (!initialized_ || !width || !height) {
    return;
  }
  if (!images_initialized_) {
    std::array<VkImageMemoryBarrier, kRingSize + 1> barriers;
    for (uint32_t i = 0; i <= kRingSize; ++i) {
      VkImageMemoryBarrier& barrier = barriers[i];
      barrier = {};
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.dstAccessMask =
          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = i < kRingSize ? frames_[i] : thumb_;
      barrier.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
    }
    command_buffer.CmdVkPipelineBarrier(
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
    images_initialized_ = true;
  }
  const uint32_t slot_index = uint32_t(frame % kRingSize);
  TransferBarrier(command_buffer);
  VkImageBlit blit = {};
  blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  blit.srcSubresource.layerCount = 1;
  blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
  blit.dstSubresource = blit.srcSubresource;
  blit.dstOffsets[1] = {int32_t(kFrameWidth), int32_t(kFrameHeight), 1};
  command_buffer.CmdVkBlitImage(guest_output, VK_IMAGE_LAYOUT_GENERAL,
                                frames_[slot_index], VK_IMAGE_LAYOUT_GENERAL,
                                1, &blit, VK_FILTER_LINEAR);
  blit.dstOffsets[1] = {int32_t(kThumbWidth), int32_t(kThumbHeight), 1};
  command_buffer.CmdVkBlitImage(guest_output, VK_IMAGE_LAYOUT_GENERAL, thumb_,
                                VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                                VK_FILTER_LINEAR);
  TransferBarrier(command_buffer);
  VkBufferImageCopy copy = {};
  copy.bufferOffset = VkDeviceSize(kThumbBytes) * slot_index;
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {kThumbWidth, kThumbHeight, 1};
  command_buffer.CmdVkCopyImageToBuffer(thumb_, VK_IMAGE_LAYOUT_GENERAL,
                                        thumb_buffer_, 1, &copy);
  Slot& slot = slots_[slot_index];
  slot.frame = frame;
  slot.submission = current_submission;
  slot.scored = false;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
