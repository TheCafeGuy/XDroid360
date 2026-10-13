// XDroid360 GPU breadcrumbs: locate the draw a GPU hang stops in (XR-1a,
// docs/design/xr-gpu-capture-replay-recovery.md). VK_AMD_buffer_marker writes
// sequence numbers into a host-visible buffer that is read after device loss.
#ifndef XENIA_GPU_VULKAN_VULKAN_GPU_BREADCRUMBS_H_
#define XENIA_GPU_VULKAN_VULKAN_GPU_BREADCRUMBS_H_

#include <cstdint>
#include <vector>

#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/ui/vulkan/vulkan_device.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanGpuBreadcrumbs {
 public:
  struct DrawInfo {
    uint64_t vs_hash;
    uint64_t ps_hash;
    uint64_t render_pass_key;
    uint32_t host_vertex_count;
    uint8_t guest_primitive_type;
    // PrimitiveProcessor::ProcessedIndexBufferType.
    uint8_t index_buffer_type;
  };

  // Compute work outside render passes (STAB-S1: every hang of 2026-10-11
  // stopped between passes, with the shader processor busy).
  enum class WorkKind : uint8_t {
    kResolveCopy,
    kResolveClear,
    kTransfer,
    kDirectResolve,
    kTextureLoad,
    kTextureBlit,
    kDump,
    kCount,
  };
  static const char* WorkKindName(WorkKind kind);

  bool Initialize(const ui::vulkan::VulkanDevice* vulkan_device);
  void Shutdown();
  bool enabled() const { return mapping_ != nullptr; }

  // Right after the render pass begin command.
  void OnRenderPassBegin(DeferredCommandBuffer& command_buffer,
                         uint64_t submission, uint64_t frame);
  // Right before and right after the guest draw command.
  void OnDrawBegin(DeferredCommandBuffer& command_buffer, const DrawInfo& info);
  void OnDrawEnd(DeferredCommandBuffer& command_buffer);
  // Right after the render pass end command.
  void OnRenderPassEnd(DeferredCommandBuffer& command_buffer);
  // Right before and right after a compute dispatch outside render passes.
  static constexpr uint32_t kWorkDetailWords = 16;
  void OnWorkBegin(DeferredCommandBuffer& command_buffer, WorkKind kind,
                   uint32_t group_count_x, uint32_t group_count_y,
                   uint32_t group_count_z, uint64_t submission, uint64_t frame,
                   const uint32_t* detail, uint32_t detail_count);
  void OnWorkEnd(DeferredCommandBuffer& command_buffer);

  // Logs where the GPU stopped. Only meaningful after device loss, when the
  // GPU no longer writes the buffer.
  void LogHang() const;

 private:
  // One slot per render pass, indexed by pass id. Values are global sequence
  // numbers (pass ids for begin/end), so a slot not yet written by its current
  // pass holds an older value and is recognized as stale.
  struct Slot {
    // TOP_OF_PIPE after the begin: the CP reached the pass.
    uint32_t begin;
    // TOP_OF_PIPE before each guest draw: the CP issued that draw.
    uint32_t issued;
    // BOTTOM_OF_PIPE after the prologue and each guest draw. Turnip replays
    // the pass commands per bin (and in the binning pass), so this is the
    // last draw completed in the bin being rendered.
    uint32_t done;
    // BOTTOM_OF_PIPE after the end: every bin of the pass finished.
    uint32_t end;
  };
  struct PassRecord {
    uint32_t pass_id;
    // Sequence number of the marker written before the first guest draw,
    // covering the tile load, ownership transfers and clears. 0 if no draw.
    uint32_t prologue_seq;
    uint32_t last_seq;
    uint32_t draw_count;
    uint64_t submission;
    uint64_t frame;
    uint64_t render_pass_key;
  };
  struct DrawRecord {
    uint32_t seq;
    uint32_t pass_id;
    DrawInfo info;
  };
  // After the pass slots: TOP_OF_PIPE before and BOTTOM_OF_PIPE after each
  // compute dispatch outside passes, as work ids.
  struct WorkSlot {
    uint32_t issued;
    uint32_t done;
  };
  struct WorkRecord {
    uint32_t work_id;
    WorkKind kind;
    uint32_t group_count[3];
    // The newest pass begun on the CPU timeline before this work.
    uint32_t after_pass_id;
    uint64_t submission;
    uint64_t frame;
    // Caller-defined inputs of the dispatch (TagDispatchDetail), word 0 being
    // its pipeline key.
    uint32_t detail_count;
    uint32_t detail[kWorkDetailWords];
  };
  // Direct resolve dispatches per pipeline key, to weigh the variant a hang
  // stops in against how often it runs.
  struct VariantCount {
    uint32_t pipeline_key;
    uint32_t count;
  };
  static constexpr uint32_t kSlotCount = 512;
  static constexpr uint32_t kDrawRingSize = 1 << 15;
  static constexpr uint32_t kWorkSlotCount = 1024;
  static constexpr uint32_t kVariantCountSlots = 32;
  void LogWorkHang() const;

  void WriteMarker(DeferredCommandBuffer& command_buffer,
                   VkPipelineStageFlagBits stage, uint32_t slot_field_offset,
                   uint32_t value) const;
  Slot ReadSlot(uint32_t pass_id) const;
  const DrawRecord* FindDraw(uint32_t seq) const;
  void LogDraw(const char* label, const DrawRecord& draw,
               const PassRecord& pass) const;

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  uint32_t memory_type_ = UINT32_MAX;
  volatile uint32_t* mapping_ = nullptr;

  uint32_t next_pass_id_ = 0;
  uint32_t next_seq_ = 0;
  // Pass currently open on the CPU timeline, 0 if none.
  uint32_t open_pass_id_ = 0;
  uint32_t open_draw_seq_ = 0;
  uint32_t next_work_id_ = 0;
  uint32_t open_work_id_ = 0;
  std::vector<PassRecord> passes_;
  std::vector<DrawRecord> draws_;
  std::vector<WorkRecord> works_;
  VariantCount direct_resolve_variants_[kVariantCountSlots] = {};
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_GPU_BREADCRUMBS_H_
