// XDroid360 GPU breadcrumbs. See vulkan_gpu_breadcrumbs.h.
#include "xenia/gpu/vulkan/vulkan_gpu_breadcrumbs.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include "xenia/base/logging.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace gpu {
namespace vulkan {

bool VulkanGpuBreadcrumbs::Initialize(
    const ui::vulkan::VulkanDevice* vulkan_device) {
  Shutdown();
  if (!vulkan_device->cmd_write_buffer_marker_amd()) {
    XELOGW("XDROID-BC: VK_AMD_buffer_marker unavailable, breadcrumbs off");
    return false;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  constexpr VkDeviceSize kBufferSize =
      sizeof(Slot) * kSlotCount + sizeof(WorkSlot) * kWorkSlotCount;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, kBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, buffer_, memory_,
          &memory_type_)) {
    XELOGE("XDROID-BC: failed to create the breadcrumb buffer");
    return false;
  }
  void* mapping;
  if (dfn.vkMapMemory(device, memory_, 0, VK_WHOLE_SIZE, 0, &mapping) !=
      VK_SUCCESS) {
    XELOGE("XDROID-BC: failed to map the breadcrumb buffer");
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, memory_);
    return false;
  }
  std::memset(mapping, 0, size_t(kBufferSize));
  ui::vulkan::util::FlushMappedMemoryRange(vulkan_device, memory_,
                                           memory_type_);
  vulkan_device_ = vulkan_device;
  mapping_ = static_cast<volatile uint32_t*>(mapping);
  next_pass_id_ = 0;
  next_seq_ = 0;
  open_pass_id_ = 0;
  open_draw_seq_ = 0;
  passes_.assign(kSlotCount, PassRecord{});
  draws_.assign(kDrawRingSize, DrawRecord{});
  next_work_id_ = 0;
  open_work_id_ = 0;
  works_.assign(kWorkSlotCount, WorkRecord{});
  XELOGI(
      "XDROID-BC: GPU breadcrumbs on ({} pass slots, {} draw records, memory "
      "type {})",
      kSlotCount, kDrawRingSize, memory_type_);
  return true;
}

void VulkanGpuBreadcrumbs::Shutdown() {
  if (vulkan_device_) {
    const ui::vulkan::VulkanDevice::Functions& dfn =
        vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    if (mapping_) {
      dfn.vkUnmapMemory(device, memory_);
    }
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, memory_);
  }
  vulkan_device_ = nullptr;
  mapping_ = nullptr;
  passes_.clear();
  passes_.shrink_to_fit();
  draws_.clear();
  draws_.shrink_to_fit();
}

void VulkanGpuBreadcrumbs::WriteMarker(DeferredCommandBuffer& command_buffer,
                                       VkPipelineStageFlagBits stage,
                                       uint32_t slot_field_offset,
                                       uint32_t value) const {
  command_buffer.CmdVkWriteBufferMarkerAMD(
      stage, buffer_,
      VkDeviceSize(sizeof(Slot)) * (open_pass_id_ % kSlotCount) +
          slot_field_offset,
      value);
}

const char* VulkanGpuBreadcrumbs::WorkKindName(WorkKind kind) {
  switch (kind) {
    case WorkKind::kResolveCopy:
      return "resolve copy";
    case WorkKind::kResolveClear:
      return "resolve clear";
    case WorkKind::kTransfer:
      return "transfer";
    case WorkKind::kDirectResolve:
      return "direct resolve";
    case WorkKind::kTextureLoad:
      return "texture load";
    case WorkKind::kTextureBlit:
      return "texture blit";
    case WorkKind::kDump:
      return "render target dump";
    default:
      return "unknown";
  }
}

void VulkanGpuBreadcrumbs::OnWorkBegin(DeferredCommandBuffer& command_buffer,
                                       WorkKind kind, uint32_t group_count_x,
                                       uint32_t group_count_y,
                                       uint32_t group_count_z,
                                       uint64_t submission, uint64_t frame,
                                       const uint32_t* detail,
                                       uint32_t detail_count) {
  if (!mapping_) {
    return;
  }
  open_work_id_ = ++next_work_id_;
  WorkRecord& work = works_[open_work_id_ % kWorkSlotCount];
  work.work_id = open_work_id_;
  work.kind = kind;
  work.group_count[0] = group_count_x;
  work.group_count[1] = group_count_y;
  work.group_count[2] = group_count_z;
  work.after_pass_id = next_pass_id_;
  work.submission = submission;
  work.frame = frame;
  work.detail_count = std::min(detail_count, kWorkDetailWords);
  if (work.detail_count) {
    std::memcpy(work.detail, detail, sizeof(uint32_t) * work.detail_count);
  }
  if (kind == WorkKind::kDirectResolve && work.detail_count) {
    // Linear probe on the pipeline key, which is never 0 for a direct resolve
    // (its direct_resolve bit is set); a full table drops new keys.
    const uint32_t key = work.detail[0];
    for (uint32_t i = 0; i < kVariantCountSlots; ++i) {
      VariantCount& variant =
          direct_resolve_variants_[(key + i) % kVariantCountSlots];
      if (variant.pipeline_key == key || !variant.pipeline_key) {
        variant.pipeline_key = key;
        ++variant.count;
        break;
      }
    }
  }
  command_buffer.CmdVkWriteBufferMarkerAMD(
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buffer_,
      VkDeviceSize(sizeof(Slot)) * kSlotCount +
          VkDeviceSize(sizeof(WorkSlot)) * (open_work_id_ % kWorkSlotCount) +
          offsetof(WorkSlot, issued),
      open_work_id_);
}

void VulkanGpuBreadcrumbs::OnWorkEnd(DeferredCommandBuffer& command_buffer) {
  if (!mapping_ || !open_work_id_) {
    return;
  }
  command_buffer.CmdVkWriteBufferMarkerAMD(
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, buffer_,
      VkDeviceSize(sizeof(Slot)) * kSlotCount +
          VkDeviceSize(sizeof(WorkSlot)) * (open_work_id_ % kWorkSlotCount) +
          offsetof(WorkSlot, done),
      open_work_id_);
  open_work_id_ = 0;
}

void VulkanGpuBreadcrumbs::LogWorkHang() const {
  if (!next_work_id_) {
    return;
  }
  const volatile uint32_t* work_slots =
      mapping_ + kSlotCount * (sizeof(Slot) / sizeof(uint32_t));
  const uint32_t last = next_work_id_;
  const uint32_t first =
      last > kWorkSlotCount ? last - kWorkSlotCount + 1 : 1;
  uint32_t newest_done = 0;
  uint32_t stuck = 0;
  for (uint32_t work_id = first; work_id <= last; ++work_id) {
    const volatile uint32_t* slot =
        work_slots + (work_id % kWorkSlotCount) * 2;
    const uint32_t issued = slot[0];
    const uint32_t done = slot[1];
    if (done == work_id) {
      newest_done = work_id;
    } else if (issued == work_id && !stuck) {
      stuck = work_id;
    }
  }
  const auto log_work = [this](const char* label, uint32_t work_id) {
    const WorkRecord& work = works_[work_id % kWorkSlotCount];
    if (work.work_id != work_id) {
      return;
    }
    XELOGE(
        "XDROID-BC: {} work {}: {} dispatch {}x{}x{} ({} groups), sub {} "
        "frame {}, after pass {}",
        label, work_id, WorkKindName(work.kind), work.group_count[0],
        work.group_count[1], work.group_count[2],
        uint64_t(work.group_count[0]) * work.group_count[1] *
            work.group_count[2],
        work.submission, work.frame, work.after_pass_id);
    if (work.detail_count) {
      std::string words;
      for (uint32_t i = 0; i < work.detail_count; ++i) {
        words += fmt::format(" {:08X}", work.detail[i]);
      }
      XELOGE("XDROID-BC: {} work {} detail:{}", label, work_id, words);
    }
  };
  XELOGE(
      "XDROID-BC: compute work {}-{} tracked, newest finished {}, first "
      "issued but unfinished {}",
      first, last, newest_done, stuck);
  if (newest_done) {
    log_work("newest finished", newest_done);
  }
  if (stuck) {
    log_work("STUCK", stuck);
  }
  std::string variants;
  for (const VariantCount& variant : direct_resolve_variants_) {
    if (variant.pipeline_key) {
      variants += fmt::format(" {:08X}:{}", variant.pipeline_key, variant.count);
    }
  }
  if (!variants.empty()) {
    XELOGE("XDROID-BC: direct resolve dispatches per pipeline key:{}",
           variants);
  }
}

void VulkanGpuBreadcrumbs::OnRenderPassBegin(
    DeferredCommandBuffer& command_buffer, uint64_t submission,
    uint64_t frame) {
  open_pass_id_ = ++next_pass_id_;
  open_draw_seq_ = 0;
  PassRecord& pass = passes_[open_pass_id_ % kSlotCount];
  pass = PassRecord{};
  pass.pass_id = open_pass_id_;
  pass.submission = submission;
  pass.frame = frame;
  WriteMarker(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              offsetof(Slot, begin), open_pass_id_);
}

void VulkanGpuBreadcrumbs::OnDrawBegin(DeferredCommandBuffer& command_buffer,
                                       const DrawInfo& info) {
  if (!open_pass_id_) {
    return;
  }
  PassRecord& pass = passes_[open_pass_id_ % kSlotCount];
  if (!pass.prologue_seq) {
    // Everything the pass did before its first guest draw (bin load,
    // ownership transfers, clears) completes before this marker.
    pass.prologue_seq = ++next_seq_;
    pass.render_pass_key = info.render_pass_key;
    WriteMarker(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                offsetof(Slot, done), pass.prologue_seq);
  }
  open_draw_seq_ = ++next_seq_;
  pass.last_seq = open_draw_seq_;
  ++pass.draw_count;
  DrawRecord& draw = draws_[open_draw_seq_ % kDrawRingSize];
  draw.seq = open_draw_seq_;
  draw.pass_id = open_pass_id_;
  draw.info = info;
  WriteMarker(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              offsetof(Slot, issued), open_draw_seq_);
}

void VulkanGpuBreadcrumbs::OnDrawEnd(DeferredCommandBuffer& command_buffer) {
  if (!open_pass_id_ || !open_draw_seq_) {
    return;
  }
  WriteMarker(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
              offsetof(Slot, done), open_draw_seq_);
  open_draw_seq_ = 0;
}

void VulkanGpuBreadcrumbs::OnRenderPassEnd(
    DeferredCommandBuffer& command_buffer) {
  if (!open_pass_id_) {
    return;
  }
  WriteMarker(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
              offsetof(Slot, end), open_pass_id_);
  open_pass_id_ = 0;
  open_draw_seq_ = 0;
}

VulkanGpuBreadcrumbs::Slot VulkanGpuBreadcrumbs::ReadSlot(
    uint32_t pass_id) const {
  const volatile uint32_t* slot =
      mapping_ + (pass_id % kSlotCount) * (sizeof(Slot) / sizeof(uint32_t));
  Slot result;
  result.begin = slot[offsetof(Slot, begin) / sizeof(uint32_t)];
  result.issued = slot[offsetof(Slot, issued) / sizeof(uint32_t)];
  result.done = slot[offsetof(Slot, done) / sizeof(uint32_t)];
  result.end = slot[offsetof(Slot, end) / sizeof(uint32_t)];
  return result;
}

const VulkanGpuBreadcrumbs::DrawRecord* VulkanGpuBreadcrumbs::FindDraw(
    uint32_t seq) const {
  const DrawRecord& draw = draws_[seq % kDrawRingSize];
  return (seq && draw.seq == seq) ? &draw : nullptr;
}

void VulkanGpuBreadcrumbs::LogDraw(const char* label, const DrawRecord& draw,
                                   const PassRecord& pass) const {
  XELOGE(
      "XDROID-BC:   {} seq {} (draw {}/{} of pass {}): VS={:016X} "
      "PS={:016X} prim {} host verts {} index {}",
      label, draw.seq, draw.seq - pass.prologue_seq, pass.draw_count,
      pass.pass_id, draw.info.vs_hash, draw.info.ps_hash,
      draw.info.guest_primitive_type, draw.info.host_vertex_count,
      draw.info.index_buffer_type);
}

void VulkanGpuBreadcrumbs::LogHang() const {
  if (!mapping_ || !next_pass_id_) {
    return;
  }
  ui::vulkan::util::InvalidateMappedMemoryRange(vulkan_device_, memory_,
                                                memory_type_);
  LogWorkHang();
  // Only the newest kSlotCount passes still own their slots.
  const uint32_t last = next_pass_id_;
  const uint32_t first = last > kSlotCount ? last - kSlotCount + 1 : 1;
  // Pass ends complete in order, so the stuck pass is the first one after the
  // newest ended pass that the CP reached. Unended passes before the newest
  // ended one would break that order and are only counted.
  uint32_t newest_ended = 0;
  for (uint32_t pass_id = first; pass_id <= last; ++pass_id) {
    if (ReadSlot(pass_id).end == pass_id) {
      newest_ended = pass_id;
    }
  }
  uint32_t unended_before = 0;
  uint32_t stuck = 0;
  uint32_t reached_after_stuck = 0;
  for (uint32_t pass_id = first; pass_id <= last; ++pass_id) {
    const Slot slot = ReadSlot(pass_id);
    if (slot.begin != pass_id || slot.end == pass_id) {
      continue;
    }
    if (pass_id < newest_ended) {
      ++unended_before;
    } else if (!stuck) {
      stuck = pass_id;
    } else {
      ++reached_after_stuck;
    }
  }
  XELOGE(
      "XDROID-BC: hang report: passes {}-{} tracked, newest ended {}, stuck "
      "{}, {} more reached after it, {} unended before the newest ended",
      first, last, newest_ended, stuck, reached_after_stuck, unended_before);
  if (newest_ended) {
    const PassRecord& pass = passes_[newest_ended % kSlotCount];
    XELOGE("XDROID-BC: newest ended pass {}: sub {} frame {} draws {}",
           newest_ended, pass.submission, pass.frame, pass.draw_count);
  }
  if (!stuck) {
    XELOGE(
        "XDROID-BC: no pass is in flight: the GPU stopped outside render "
        "passes (copy, resolve or dispatch) after pass {}",
        newest_ended);
    return;
  }
  const PassRecord& pass = passes_[stuck % kSlotCount];
  const Slot slot = ReadSlot(stuck);
  XELOGE(
      "XDROID-BC: stuck pass {}: sub {} frame {} RP=0x{:08X} guest draws {} "
      "(seq {}-{}), done {} issued {}",
      stuck, pass.submission, pass.frame, uint32_t(pass.render_pass_key),
      pass.draw_count, pass.prologue_seq, pass.last_seq, slot.done,
      slot.issued);
  // The draw to show the neighbourhood of, and how to label it.
  uint32_t suspect = 0;
  const char* suspect_label = "SUSPECT";
  if (!pass.prologue_seq) {
    XELOGE(
        "XDROID-BC: verdict: the pass has no guest draws (transfer or clear "
        "pass); it stopped inside it");
  } else if (slot.done < pass.prologue_seq) {
    XELOGE(
        "XDROID-BC: verdict: stopped before the first guest draw - bin load, "
        "ownership transfers or clears of this pass, or binning start");
    suspect = pass.prologue_seq + 1;
    suspect_label = "first";
  } else if (slot.done >= pass.last_seq) {
    XELOGE(
        "XDROID-BC: verdict: every guest draw finished in the current bin - "
        "stopped in the bin store/resolve or before the next bin's draws");
  } else {
    suspect = slot.done + 1;
    XELOGE(
        "XDROID-BC: verdict: draw {}/{} did not finish (last finished {}, CP "
        "issued up to {})",
        suspect - pass.prologue_seq, pass.draw_count,
        slot.done - pass.prologue_seq,
        slot.issued >= pass.prologue_seq ? slot.issued - pass.prologue_seq
                                         : 0);
  }
  if (suspect) {
    const uint32_t from =
        suspect > pass.prologue_seq + 3 ? suspect - 3 : pass.prologue_seq + 1;
    const uint32_t to = std::min(pass.last_seq, suspect + 2);
    for (uint32_t seq = from; seq <= to; ++seq) {
      const DrawRecord* draw = FindDraw(seq);
      if (draw) {
        LogDraw(seq == suspect ? suspect_label : "near", *draw, pass);
      }
    }
  }
  // Passes the CP (or the concurrent binning pipe) already started past the
  // stuck one.
  uint32_t logged = 0;
  for (uint32_t pass_id = stuck + 1; pass_id <= last && logged < 4;
       ++pass_id) {
    const Slot later = ReadSlot(pass_id);
    if (later.begin != pass_id || later.end == pass_id) {
      continue;
    }
    const PassRecord& later_pass = passes_[pass_id % kSlotCount];
    XELOGE(
        "XDROID-BC: also reached pass {}: sub {} draws {} (seq {}-{}), done "
        "{} issued {}",
        pass_id, later_pass.submission, later_pass.draw_count,
        later_pass.prologue_seq, later_pass.last_seq, later.done,
        later.issued);
    ++logged;
  }
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
