/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_COMMAND_PROCESSOR_H_
#define XENIA_GPU_VULKAN_VULKAN_COMMAND_PROCESSOR_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/hash.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_draw_blackbox.h"
#include "xenia/gpu/vulkan/vulkan_glitch_recorder.h"
#include "xenia/gpu/vulkan/vulkan_gpu_breadcrumbs.h"
#include "xenia/gpu/vulkan/vulkan_gpu_sanity.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"
#include "xenia/gpu/vulkan/vulkan_primitive_processor.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/vulkan/vulkan_texture_cache.h"
#include "xenia/gpu/vulkan/vulkan_zpd_query_pool.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/vulkan/linked_type_descriptor_set_allocator.h"
#include "xenia/ui/vulkan/vulkan_descriptor_pool_chain.h"
#include "xenia/ui/vulkan/vulkan_gpu_completion_timeline.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_provider.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor final : public CommandProcessor {
 protected:
#define OVERRIDING_BASE_CMDPROCESSOR
#include "../pm4_command_processor_declare.h"
#undef OVERRIDING_BASE_CMDPROCESSOR
 public:
  // Single-descriptor layouts for use within a single frame.
  enum class SingleTransientDescriptorLayout {
    kStorageBuffer,
    // Scratch buffer plus the destination image of a texture load, for the
    // compute blit that replaces vkCmdCopyBufferToImage.
    kStorageBufferAndStorageImage,
    kCount,
  };

  class ScratchBufferAcquisition {
   public:
    explicit ScratchBufferAcquisition() = default;
    explicit ScratchBufferAcquisition(VulkanCommandProcessor& command_processor,
                                      VkBuffer buffer,
                                      VkPipelineStageFlags stage_mask,
                                      VkAccessFlags access_mask)
        : command_processor_(&command_processor),
          buffer_(buffer),
          stage_mask_(stage_mask),
          access_mask_(access_mask) {}

    ScratchBufferAcquisition(const ScratchBufferAcquisition& acquisition) =
        delete;
    ScratchBufferAcquisition& operator=(
        const ScratchBufferAcquisition& acquisition) = delete;

    ScratchBufferAcquisition(ScratchBufferAcquisition&& acquisition) {
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
    }
    ScratchBufferAcquisition& operator=(
        ScratchBufferAcquisition&& acquisition) {
      if (this == &acquisition) {
        return *this;
      }
      command_processor_ = acquisition.command_processor_;
      buffer_ = acquisition.buffer_;
      stage_mask_ = acquisition.stage_mask_;
      access_mask_ = acquisition.access_mask_;
      acquisition.command_processor_ = nullptr;
      acquisition.buffer_ = VK_NULL_HANDLE;
      acquisition.stage_mask_ = 0;
      acquisition.access_mask_ = 0;
      return *this;
    }

    ~ScratchBufferAcquisition() {
      if (buffer_ != VK_NULL_HANDLE) {
        assert_true(command_processor_->scratch_buffer_used_);
        assert_true(command_processor_->scratch_buffer_ == buffer_);
        command_processor_->scratch_buffer_last_stage_mask_ = stage_mask_;
        command_processor_->scratch_buffer_last_access_mask_ = access_mask_;
        command_processor_->scratch_buffer_last_usage_submission_ =
            command_processor_->GetCurrentSubmission();
        command_processor_->scratch_buffer_used_ = false;
      }
    }

    // VK_NULL_HANDLE if failed to acquire or if moved.
    VkBuffer buffer() const { return buffer_; }

    VkPipelineStageFlags GetStageMask() const { return stage_mask_; }
    VkPipelineStageFlags SetStageMask(VkPipelineStageFlags new_stage_mask) {
      VkPipelineStageFlags old_stage_mask = stage_mask_;
      stage_mask_ = new_stage_mask;
      return old_stage_mask;
    }
    VkAccessFlags GetAccessMask() const { return access_mask_; }
    VkAccessFlags SetAccessMask(VkAccessFlags new_access_mask) {
      VkAccessFlags old_access_mask = access_mask_;
      access_mask_ = new_access_mask;
      return old_access_mask;
    }

   private:
    VulkanCommandProcessor* command_processor_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkPipelineStageFlags stage_mask_ = 0;
    VkAccessFlags access_mask_ = 0;
  };

  VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system,
                         kernel::KernelState* kernel_state);
  ~VulkanCommandProcessor();

  void ClearCaches() override;
  void InvalidateGpuMemory() override;
  void ClearReadbackBuffers() override;

  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;

  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr) override;

  void RestoreEdramSnapshot(const void* snapshot) override;

  void PollCompletedSubmission() override;

  ui::vulkan::VulkanDevice* GetVulkanDevice() const {
    return static_cast<const ui::vulkan::VulkanProvider*>(
               graphics_system_->provider())
        ->vulkan_device();
  }

  // Returns the deferred drawing command list for the currently open
  // submission.
  DeferredCommandBuffer& deferred_command_buffer() {
    assert_true(submission_open_);
    return deferred_command_buffer_;
  }

  bool submission_open() const { return submission_open_; }
  uint64_t GetCurrentSubmission() const {
    return completion_timeline_.GetUpcomingSubmission();
  }
  uint64_t GetCompletedSubmission() const override {
    return completion_timeline_.GetCompletedSubmissionFromLastUpdate();
  }

  // Sparse binds are:
  // - In a single submission, all submitted in one vkQueueBindSparse.
  // - Sent to the queue without waiting for a semaphore.
  // Thus, multiple sparse binds between the completed and the current
  // submission, and within one submission, must not touch any overlapping
  // memory regions.
  void SparseBindBuffer(VkBuffer buffer, uint32_t bind_count,
                        const VkSparseMemoryBind* binds,
                        VkPipelineStageFlags wait_stage_mask);

  uint64_t GetCurrentFrame() const { return frame_current_; }
  uint64_t GetCompletedFrame() const { return frame_completed_; }

  // Submission must be open to insert barriers. If no pipeline stages access
  // the resource in a synchronization scope, the stage masks should be 0 (top /
  // bottom of pipe should be specified only if explicitly needed). Returning
  // true if the barrier has actually been inserted and not dropped.
  bool PushBufferMemoryBarrier(
      VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
      VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
      VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
      uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      bool skip_if_equal = true, uint32_t site_line = __builtin_LINE(),
      const char* site_file = __builtin_FILE());
  bool PushImageMemoryBarrier(
      VkImage image, const VkImageSubresourceRange& subresource_range,
      VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
      VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
      VkImageLayout old_layout, VkImageLayout new_layout,
      uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
      bool skip_if_equal = true, uint32_t site_line = __builtin_LINE(),
      const char* site_file = __builtin_FILE());
  // Returns whether any barriers have been submitted - if true is returned, the
  // render pass will also be closed.
  bool SubmitBarriers(bool force_end_render_pass);

  // GE-0 (XDroid360): why the next EndRenderPass ends a pass. Set right before
  // a SubmitBarriers / EndRenderPass call; consumed (reset to kOther) by the
  // next EndRenderPass, and by a SubmitBarriers that ends nothing.
  enum class PassEndReason : uint8_t {
    kOther,
    kRtChange,
    kBarrier,
    kTextureLoad,
    kUpload,
    kResolve,
    kTransfer,
    kReadback,
    kQuery,
    kSwap,
    kSubmit,
    kCount,
  };
  void TagPassEnd(PassEndReason reason) { pass_end_reason_ = reason; }

  // GE-4 pass splice (xdroid_pass_splice bits). Work that would end the open
  // guest render pass (a texture load, a shared-memory upload, a resolve) can
  // instead be recorded into a segment that runs right before that pass's
  // begin, when nothing the pass's earlier draws used is touched by it.
  enum SpliceKind : uint32_t {
    kSpliceTextureLoads = 1,
    kSpliceUploads = 2,
    kSpliceResolves = 4,
  };
  // Draw serials: each recorded guest draw gets the current value, which then
  // advances, so "used by an earlier draw of the open pass" is a range check.
  uint64_t draw_serial() const { return draw_serial_; }
  uint64_t pass_first_draw_serial() const { return pass_first_draw_serial_; }
  bool IsDrawSerialEarlierInPass(uint64_t serial) const {
    return serial >= pass_first_draw_serial_ && serial < draw_serial_;
  }
  // Whether this kind of work may be spliced into the open pass now (the
  // caller still checks its own resources).
  bool IsSpliceCandidate(SpliceKind kind) const;
  // Whether a render target (RenderTargetCache::RenderTarget*) is bound in the
  // open guest pass.
  bool IsRenderTargetBoundInSplicePass(const void* render_target) const;
  // On true, the following commands go to the segment until EndSplice; the
  // caller must not record anything that belongs inside the pass meanwhile.
  // On false nothing changed, and the caller ends the pass as it used to.
  bool BeginSplice(SpliceKind kind);
  void EndSplice();
  bool is_splice_recording() const { return splice_depth_ != 0; }
  // An image written by spliced work, so its later barriers may join the
  // segment too.
  void NoteSplicedImage(VkImage image);

  // STAB-S1: every GPU hang of 2026-10-11 stopped between render passes with
  // the shader processor busy, so compute dispatches get breadcrumbs too.
  // Callers name the next dispatch; DeferredCommandBuffer::CmdVkDispatch
  // calls the hooks around it.
  void TagDispatch(VulkanGpuBreadcrumbs::WorkKind kind) {
    dispatch_kind_ = kind;
  }
  // Inputs of the next dispatch for the hang report, word 0 its pipeline key.
  // Only worth building while breadcrumbs are on.
  bool gpu_breadcrumbs_enabled() const { return gpu_breadcrumbs_.enabled(); }
  void TagDispatchDetail(const uint32_t* detail, uint32_t count) {
    dispatch_detail_count_ =
        std::min(count, VulkanGpuBreadcrumbs::kWorkDetailWords);
    std::memcpy(dispatch_detail_, detail,
                sizeof(uint32_t) * dispatch_detail_count_);
  }
  void OnDispatchBegin(DeferredCommandBuffer& command_buffer,
                       uint32_t group_count_x, uint32_t group_count_y,
                       uint32_t group_count_z);
  void OnDispatchEnd(DeferredCommandBuffer& command_buffer);

  // If [base_bytes, base_bytes + size_bytes) holds memexport output (which
  // under two-buffer routing lives in the host-imported buffer aliasing guest
  // RAM), copies the pages holding it into the device-local buffer so a
  // following device-buffer read - a texture load - sees it, or all of the
  // range with whole_range. No-op for non-memexport ranges (already in the
  // device buffer) and when the host buffer is unavailable. Called by the
  // texture cache before loading a texture. Ends the render pass.
  // Returns whether it copied, so an upload of the same range can be skipped.
  bool EnsureMemexportRangeInDeviceBuffer(uint32_t base_bytes,
                                          uint32_t size_bytes,
                                          bool whole_range = false);

  // If not started yet, begins a render pass from the render target cache.
  // Submission must be open.
  void SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass,
      const VulkanRenderTargetCache::Framebuffer* framebuffer);
  // Overload for transfer operations with dynamic rendering.
  // transfer_dest_view is the destination render target view for the transfer.
  // Returns whether a new pass was begun. GE-2 (xdroid_clear_pass_mode): a
  // pass that will only hold a resolve clear may pass render_area (must
  // contain the clear rect; such a pass is never reused for later work), and
  // load_op_clear_value to clear through the load op instead of a
  // vkCmdClearAttachments (dynamic rendering only; *load_op_clear_applied
  // tells whether it was used, otherwise the caller still clears).
  bool SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      VkRenderPass render_pass,
      const VulkanRenderTargetCache::Framebuffer* framebuffer,
      VkImageView transfer_dest_view, bool transfer_dest_is_depth,
      const VkRect2D* render_area = nullptr,
      const VkClearValue* load_op_clear_value = nullptr,
      bool* load_op_clear_applied = nullptr);
  // Must be called before doing anything outside the render pass scope,
  // including adding pipeline barriers that are not a part of the render pass
  // scope. Submission must be open.
  void EndRenderPass();

  VkDescriptorSetLayout GetSingleTransientDescriptorLayout(
      SingleTransientDescriptorLayout transient_descriptor_layout) const {
    return descriptor_set_layouts_single_transient_[size_t(
        transient_descriptor_layout)];
  }
  // Queues an image, its view and its memory for destruction once the
  // submission using them now has completed. Any of them may be null.
  void DestroyScratchImageWhenIdle(VkImage image, VkImageView image_view,
                                   VkDeviceMemory memory);

  // A frame must be open.
  VkDescriptorSet AllocateSingleTransientDescriptor(
      SingleTransientDescriptorLayout transient_descriptor_layout);

  // The returned reference is valid until a cache clear.
  VkDescriptorSetLayout GetTextureDescriptorSetLayout(bool is_vertex,
                                                      size_t texture_count,
                                                      size_t sampler_count);
  // The returned reference is valid until a cache clear.
  const VulkanPipelineCache::PipelineLayoutProvider* GetPipelineLayout(
      size_t texture_count_pixel, size_t sampler_count_pixel,
      size_t texture_count_vertex, size_t sampler_count_vertex);

  // Returns a single temporary GPU-side buffer within a submission for tasks
  // like texture untiling and resolving. May push a buffer memory barrier into
  // the initial usage. Submission must be open.
  ScratchBufferAcquisition AcquireScratchGpuBuffer(
      VkDeviceSize size, VkPipelineStageFlags initial_stage_mask,
      VkAccessFlags initial_access_mask);

  // Binds a graphics pipeline for host-specific purposes, invalidating the
  // affected state. keep_dynamic_* must be false (to invalidate the dynamic
  // state after binding the pipeline with the same state being static, or if
  // the caller changes the dynamic state bypassing the VulkanCommandProcessor)
  // unless the caller has these state variables as dynamic and uses the
  // tracking in VulkanCommandProcessor to modify them.
  void BindExternalGraphicsPipeline(VkPipeline pipeline,
                                    bool keep_dynamic_depth_bias = false,
                                    bool keep_dynamic_blend_constants = false,
                                    bool keep_dynamic_stencil_mask_ref = false);
  void BindExternalComputePipeline(VkPipeline pipeline);
  void SetViewport(const VkViewport& viewport);
  void SetScissor(const VkRect2D& scissor);

  std::string GetTitleStateSuffix() const override;

  // Debug marker methods - public so subsystems can annotate their operations.
  void PushDebugMarker(const char* format, ...);
  void PopDebugMarker();
  void InsertDebugMarker(const char* format, ...);
  bool debug_markers_enabled() const { return debug_markers_enabled_; }

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  XE_FORCEINLINE
  void WriteRegister(uint32_t index, uint32_t value) override;
  XE_FORCEINLINE
  virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                     uint32_t num_registers) override;

  void OnGammaRamp256EntryTableValueWritten() override;
  void OnGammaRampPWLValueWritten() override;

  // For command_processor_resolve_readwatch.inc.
  bool EndResolveSubmission() {
    return !submission_open_ || EndSubmission(false);
  }
  uint64_t GetResolveSubmittedThrough() const {
    return resolve_submitted_through_.load(std::memory_order_acquire);
  }
  // Copies resolve output into guest RAM from a fault, on its own command
  // buffer and fence, as the faulting thread can't use the GPU thread's. With
  // zero-copy, only waits for the resolve to have written it.
  bool FaultCopyResolveToGuestRam(
      const std::vector<std::pair<uint32_t, uint32_t>>& ranges);
  void DestroyResolveFaultCopy();
  bool EnsureResolveFaultReadback(uint32_t size);
  // The last submission handed to the GPU, for other threads.
  std::atomic<uint64_t> resolve_submitted_through_{0};
  VkCommandPool resolve_fault_command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer resolve_fault_command_buffer_ = VK_NULL_HANDLE;
  VkFence resolve_fault_fence_ = VK_NULL_HANDLE;
  std::vector<VkBufferCopy> resolve_fault_copy_regions_;

  // Staging storage for command_processor_readback_staging.inc, which owns the
  // pool itself. Used only without the guest RAM host buffer.
  struct ReadbackStagingBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint32_t memory_type = 0;
    VkDeviceSize memory_size = 0;
  };
  bool CreateReadbackStagingBuffer(ReadbackStagingBuffer& buffer,
                                   uint32_t size);
  void DestroyReadbackStagingBuffer(ReadbackStagingBuffer& buffer);
  // Where a fault copy lands without the guest RAM host buffer. Idle between
  // fault copies, each of which is awaited.
  ReadbackStagingBuffer resolve_fault_readback_;
  uint32_t resolve_fault_readback_size_ = 0;
  void PrepareReadbackStagingEviction();
  void InvalidateReadbackStaging(const ReadbackStagingBuffer& buffer);
  // The staging pool, shared with the D3D12 backend. Included here rather than
  // with the other fragments because the declarations below name its
  // ReadbackStagingSlot.
#include "../command_processor_readback_staging.inc"
  void OrderReadbackStagingWrite(VkBuffer staging_buffer);
  void StageMemexportReadback();
  void FlushMemexportStagingReadback();
  // Export ranges staged but not yet copied out, in record order - a later
  // copy of an overlapping range has to win.
  struct MemexportStagedRange {
    uint64_t key;
    uint32_t address;
    uint32_t length;
    // Where address lies in the slot's buffer.
    uint32_t offset;
  };
  std::vector<MemexportStagedRange> memexport_staged_;
  // For EnsureMemexportRangeInDeviceBuffer.
  std::vector<VkBufferCopy> memexport_copy_regions_;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  void OnPrimaryBufferEnd() override;

  Shader* LoadShader(xenos::ShaderType shader_type,
                     const uint32_t* host_address,
                     uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override;
  bool IssueCopy() override;
  // MSAA-1X (gpu_force_msaa_1x): clears the MSAA sample count in
  // RB_SURFACE_INFO before a draw or a resolve uses it.
  void ApplyForceMsaa1x();

  void InitializeTrace() override;
  bool DumpEdramSnapshotToFile(const std::filesystem::path& path) override;

 private:
  struct CommandBuffer {
    VkCommandPool pool;
    VkCommandBuffer buffer;
  };

  struct SparseBufferBind {
    VkBuffer buffer;
    size_t bind_offset;
    uint32_t bind_count;
  };

  union TextureDescriptorSetLayoutKey {
    uint32_t key;
    struct {
      // If texture and sampler counts are both 0, use
      // descriptor_set_layout_empty_ instead as these are owning references.
      uint32_t texture_count : 16;
      uint32_t sampler_count : 15;
      uint32_t is_vertex : 1;
    };

    TextureDescriptorSetLayoutKey() : key(0) {
      static_assert_size(*this, sizeof(key));
    }

    struct Hasher {
      size_t operator()(const TextureDescriptorSetLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const TextureDescriptorSetLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const TextureDescriptorSetLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  union PipelineLayoutKey {
    uint64_t key;
    struct {
      // Pixel textures in the low bits since those are varied much more
      // commonly.
      uint16_t texture_count_pixel;
      uint16_t sampler_count_pixel;
      uint16_t texture_count_vertex;
      uint16_t sampler_count_vertex;
    };

    PipelineLayoutKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const PipelineLayoutKey& key) const {
        return std::hash<decltype(key.key)>{}(key.key);
      }
    };
    bool operator==(const PipelineLayoutKey& other_key) const {
      return key == other_key.key;
    }
    bool operator!=(const PipelineLayoutKey& other_key) const {
      return !(*this == other_key);
    }
  };

  class PipelineLayout : public VulkanPipelineCache::PipelineLayoutProvider {
   public:
    explicit PipelineLayout(
        VkPipelineLayout pipeline_layout,
        VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref,
        VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref)
        : pipeline_layout_(pipeline_layout),
          descriptor_set_layout_textures_vertex_ref_(
              descriptor_set_layout_textures_vertex_ref),
          descriptor_set_layout_textures_pixel_ref_(
              descriptor_set_layout_textures_pixel_ref) {}
    VkPipelineLayout GetPipelineLayout() const override {
      return pipeline_layout_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref() const {
      return descriptor_set_layout_textures_vertex_ref_;
    }
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref() const {
      return descriptor_set_layout_textures_pixel_ref_;
    }

   private:
    VkPipelineLayout pipeline_layout_;
    VkDescriptorSetLayout descriptor_set_layout_textures_vertex_ref_;
    VkDescriptorSetLayout descriptor_set_layout_textures_pixel_ref_;
  };

  struct UsedSingleTransientDescriptor {
    uint64_t frame;
    SingleTransientDescriptorLayout layout;
    VkDescriptorSet set;
  };

  struct UsedTextureTransientDescriptorSet {
    uint64_t frame;
    TextureDescriptorSetLayoutKey layout;
    VkDescriptorSet set;
  };

  enum SwapApplyGammaDescriptorSet : uint32_t {
    kSwapApplyGammaDescriptorSetRamp,
    kSwapApplyGammaDescriptorSetSource,
    kSwapApplyGammaDescriptorSetDest,

    kSwapApplyGammaDescriptorSetCount,
  };

  // BeginSubmission and EndSubmission may be called at any time. If there's an
  // open non-frame submission, BeginSubmission(true) will promote it to a
  // frame. EndSubmission(true) will close the frame no matter whether the
  // submission has already been closed.
  // Unlike on Direct3D 12, submission boundaries do not imply any memory
  // barriers aside from an incoming host write (but not outgoing host read)
  // dependency.

  // Rechecks submission number and reclaims per-submission resources. Pass 0 as
  // the submission to await to simply check status, or pass
  // GetCurrentSubmission() to wait for all queue operations to be completed.
  void CheckSubmissionCompletionAndDeviceLoss(uint64_t await_submission);
  // If is_guest_command is true, a new full frame - with full cleanup of
  // resources and, if needed, starting capturing - is opened if pending (as
  // opposed to simply resuming after mid-frame synchronization). Returns
  // whether a submission is open currently and the device is not lost.
  bool BeginSubmission(bool is_guest_command);
  // If is_swap is true, a full frame is closed - with, if needed, cache
  // clearing and stopping capturing. Returns whether the submission was done
  // successfully, if it has failed, leaves it open.
  bool EndSubmission(bool is_swap);
  // Checks if ending a submission right now would not cause potentially more
  // delay than it would reduce - such as when there are unfinished graphics
  // pipeline creation requests.
  bool CanEndSubmissionImmediately() const;
  bool AwaitAllQueueOperationsCompletion() {
    CheckSubmissionCompletionAndDeviceLoss(GetCurrentSubmission());
    return !submission_open_ &&
           GetCompletedSubmission() + 1u >= GetCurrentSubmission();
  }

  void ClearTransientDescriptorPools();

  void SplitPendingBarrier();

  void DestroyScratchBuffer();

  // ZPD occlusion queries backend.
  // vkCmdBeginQuery is only valid inside a render pass, so segments split at
  // pass end and resume at the next pass begin. If a report starts outside a
  // pass, segment_pending_begin waits for the next. FSI queries clear a
  // dedicated counter with vkCmdFillBuffer, so they may need to open before a
  // pass begins or split an active pass around the clear.
  void EnsureZPDQueryResources() override;
  void ShutdownZPDQueryResources() override {
    zpd_resolves_in_flight_.clear();
    zpd_active_query_index_ = UINT32_MAX;
    zpd_active_query_generation_ = 0;
    zpd_active_query_is_fsi_ = false;
    zpd_fsi_path_ = false;
    zpd_counter_index_force_update_ = true;
    if (zpd_host_query_pool_) {
      zpd_host_query_pool_->Shutdown();
    }
  }

  bool IsZPDQueryPoolReady() const override;
  bool CanOpenZPDQuery() const override;

  QueryOpenResult OpenZPDQuery(bool can_close_submission) override;
  bool CloseZPDQuery(ReportHandle report_handle,
                     uint64_t& out_submission) override;
  void PumpQueryResolves() override;
  bool AwaitQueryResolve(ReportHandle report_handle,
                         uint64_t wait_for_submission) override;

  void UpdateDynamicState(const draw_util::ViewportInfo& viewport_info,
                          bool primitive_polygonal,
                          reg::RB_DEPTHCONTROL normalized_depth_control,
                          uint32_t draw_resolution_scale_x,
                          uint32_t draw_resolution_scale_y,
                          bool depth_bias_in_pixel_shader);
  void UpdateSystemConstantValues(
      bool primitive_polygonal,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool shader_32bit_index_dma, const draw_util::ViewportInfo& viewport_info,
      uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask,
      const draw_util::HostDepthPolygonOffset* host_depth_polygon_offset);
  bool UpdateBindings(const VulkanShader* vertex_shader,
                      const VulkanShader* pixel_shader,
                      bool interpreter_placeholder,
                      bool placeholder_pixel_shader);
  // Allocates a descriptor set and fills one or two VkWriteDescriptorSet
  // structure instances (for images and samplers).
  // The descriptor set layout must be the one for the given is_vertex,
  // texture_count, sampler_count (from GetTextureDescriptorSetLayout - may be
  // already available at the moment of the call, no need to locate it again).
  // Returns how many VkWriteDescriptorSet structure instances have been
  // written, or 0 if there was a failure to allocate the descriptor set or no
  // bindings were requested.
  uint32_t WriteTransientTextureBindings(
      bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
      VkDescriptorSetLayout descriptor_set_layout,
      const VkDescriptorImageInfo* texture_image_info,
      const VkDescriptorImageInfo* sampler_image_info,
      VkWriteDescriptorSet* descriptor_set_writes_out);

  bool device_lost_ = false;

  // Rolling per-submission summary for diagnosing VK_ERROR_DEVICE_LOST.
  // Accumulated into submission_in_progress_ during a submission, snapshotted
  // into submission_history_ on successful submit, dumped on device-loss.
  struct SubmissionSummary {
    uint64_t submission_index = 0;
    uint64_t frame_index = 0;
    uint32_t draw_count = 0;
    uint32_t dispatch_count = 0;
    uint32_t resolve_count = 0;
    uint64_t last_vs_hash = 0;
    uint64_t last_ps_hash = 0;
    uint64_t last_render_pass_key = 0;
  };
  static constexpr size_t kSubmissionHistorySize = 16;
  std::array<SubmissionSummary, kSubmissionHistorySize> submission_history_{};
  size_t submission_history_next_ = 0;
  SubmissionSummary submission_in_progress_{};
  // Per-draw GPU markers locating a GPU hang (vulkan_gpu_breadcrumbs).
  VulkanGpuBreadcrumbs gpu_breadcrumbs_;
  VulkanGlitchRecorder glitch_recorder_;
  // HUD-1 per-draw state ring (xdroid_draw_blackbox, temp).
  VulkanDrawBlackbox draw_blackbox_;
  // GPU-SAN (xdroid_gpu_sanity): the draw about to be recorded, and a drop of
  // it reported as an event.
  VulkanGpuSanity::DrawId XdroidSanDrawId(
      const VulkanShader* vertex_shader,
      const VulkanShader* pixel_shader) const;
  // XR-4 Phase 0: runs of back-to-back mergeable resolves (index = run
  // length, last = that or longer; index 0 counts resolves that are not
  // mergeable).
  void XdroidNoteResolve(bool mergeable);
  void XdroidEndResolveRun();
  uint32_t resolve_run_ = 0;
  std::array<uint64_t, 10> resolve_runs_total_{};
  std::array<uint64_t, 10> resolve_runs_window_start_{};
  void XdroidSanDrop(const char* reason, int severity,
                     const VulkanShader* vertex_shader,
                     const VulkanShader* pixel_shader) const;

  // XDroid360 SHADER-1: shader/pipeline compile stalls on this thread in the
  // current frame, logged at the frame's end with xdroid_shader_stall_log.
  struct ShaderStallFrame {
    // Shader translation on the draw thread (no async path for the draw).
    uint32_t translate_count = 0;
    uint64_t translate_ticks = 0;
    // A draw that can't use a stand-in waiting for its real pipeline.
    uint32_t await_count = 0;
    uint64_t await_ticks = 0;
    // A query resolve draining pipeline creation.
    uint32_t query_drain_count = 0;
    uint64_t query_drain_ticks = 0;
    // Draws skipped because their pipeline isn't ready (pop-in, not a stall).
    uint32_t skipped_draws = 0;
    // Early submits at a primary buffer end held back by pipeline creation.
    uint32_t submit_deferred = 0;
  };
  ShaderStallFrame shader_stall_frame_;
  // Running totals between summaries.
  uint64_t shader_stall_summary_frames_ = 0;
  uint64_t shader_stall_summary_stalled_frames_ = 0;
  uint64_t shader_stall_summary_hitch_frames_ = 0;
  uint64_t shader_stall_summary_ticks_ = 0;
  uint64_t shader_stall_summary_worst_ticks_ = 0;
  uint64_t shader_stall_summary_skipped_draws_ = 0;
  void EndFrameShaderStalls();
  // GPU stall reports (vulkan_gpu_stall_report_ms): one per stuck
  // submission, capped per session.
  static constexpr uint32_t kMaxStallReports = 32;
  // GPU thread watchdog (vulkan_gpu_stall_watchdog_ms): the stall report above
  // only runs while this thread awaits a submission, but in the hangs that
  // kill the app it blocks earlier (acquiring a swapchain image whose previous
  // paint never finished), so a separate thread watches a heartbeat and
  // reports while the GPU is still stuck. Lock-free.
  void StallWatchdogLoop();
  std::thread stall_watchdog_thread_;
  std::atomic<bool> stall_watchdog_quit_{false};
  // Steady-clock milliseconds of the last swap or submission end.
  std::atomic<uint64_t> gpu_thread_heartbeat_ms_{0};
  uint64_t stall_reported_submission_ = 0;
  uint32_t stall_reports_ = 0;
  // Host frame times of the current stats window (IssueSwap).
  static constexpr uint32_t kStatsWindowFrames = 60;
  std::array<uint32_t, kStatsWindowFrames> stats_frame_us_{};
  uint32_t stats_frame_count_ = 0;
  std::chrono::steady_clock::time_point stats_last_swap_{};
  // Tight render area of the open guest render pass.
  bool tight_area_pending_ = false;
  bool tight_area_dynamic_ = false;
  bool tight_area_valid_ = false;
  size_t tight_area_args_offset_ = 0;
  VkExtent2D tight_area_extent_{};
  int32_t tight_area_x1_ = 0, tight_area_y1_ = 0;

  void LogRecentSubmissions(const char* context);

  bool cache_clear_requested_ = false;

  // Host shader types that guest shaders can be translated into - they can
  // access the shared memory (via vertex fetch, memory export, or manual index
  // buffer reading) and textures.
  VkPipelineStageFlags guest_shader_pipeline_stages_ = 0;
  VkShaderStageFlags guest_shader_vertex_stages_ = 0;

  std::vector<VkSemaphore> semaphores_free_;

  struct PendingQueryResolve {
    uint64_t submission = 0;
    uint32_t query_index = UINT32_MAX;
    uint32_t query_generation = 0;
    uint32_t scale_area = 1;
    bool fsi = false;
    bool hybrid = false;
    ReportHandle report_handle = kInvalidReportHandle;
  };
  uint32_t zpd_active_query_index_ = UINT32_MAX;
  uint32_t zpd_active_query_generation_ = 0;
  bool zpd_active_query_is_fsi_ = false;
  bool zpd_fsi_path_ = false;
  bool zpd_hybrid_supported_ = false;
  bool zpd_counter_index_force_update_ = true;
  std::deque<PendingQueryResolve> zpd_resolves_in_flight_;
  // Fallback buffer for ZPD counter descriptor binding 1.
  VkBuffer zpd_counter_sink_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory zpd_counter_sink_buffer_memory_ = VK_NULL_HANDLE;
  // Currently installed binding 1 buffer.
  VkBuffer zpd_counter_descriptor_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize zpd_counter_descriptor_range_ = 0;

  ui::vulkan::VulkanGPUCompletionTimeline completion_timeline_;
  bool submission_open_ = false;
  // In case vkQueueSubmit fails after something like a successful
  // vkQueueBindSparse, to wait correctly on the next attempt.
  std::vector<VkSemaphore> current_submission_wait_semaphores_;
  std::vector<VkPipelineStageFlags> current_submission_wait_stage_masks_;
  std::deque<std::pair<uint64_t, VkSemaphore>>
      submissions_in_flight_semaphores_;

  static constexpr uint32_t kMaxFramesInFlight = 3;
  bool frame_open_ = false;
  // Comparable against the Metal backend's render encoder count: on a TBDR GPU
  // each one is a tile store plus an attachment reload.
  uint64_t render_passes_total_ = 0;
  uint64_t render_passes_window_start_ = 0;
  // MSAA-1X: draws per guest sample count (index = xenos::MsaaSamples) and
  // draws/resolves turned into 1x by gpu_force_msaa_1x.
  std::array<uint64_t, 4> msaa_draws_total_ = {};
  // TILE-1 bound: predicated tile index of the current draw (-1 = untiled).
  uint64_t xdroid_tile_last_select_ = ~uint64_t(0);
  int32_t xdroid_tile_index_ = -1;
  bool xdroid_tile_second_checked_ = false;
  // TILE-1: a frame merges only after this many frames whose first tile
  // bound one set of render targets and resolved only at its end.
  uint32_t xdroid_tile_clean_streak_ = 0;
  bool xdroid_tile0_resolved_ = false;
  bool xdroid_tile0_draw_after_resolve_ = false;
  uint32_t xdroid_tile_aborts_seen_ = 0;
  // Per-window counts for the XDROID-TILE stats line.
  uint32_t xdroid_tile_stat_frames_ = 0;
  uint32_t xdroid_tile_stat_merged_ = 0;
  uint32_t xdroid_tile_stat_rt_change_ = 0;
  uint32_t xdroid_tile_stat_offset_ = 0;
  uint32_t xdroid_tile_stat_other_ = 0;
  void XdroidTileChanged(int32_t from, int32_t to);
  void XdroidTileCheckSecond();
  std::array<uint64_t, 4> msaa_draws_window_start_ = {};
  uint64_t msaa_forced_total_ = 0;
  uint64_t msaa_forced_window_start_ = 0;
  uint32_t render_pass_window_frames_ = 0;
  uint64_t vblank_window_start_ = 0;
  PassEndReason pass_end_reason_ = PassEndReason::kOther;
  VulkanGpuBreadcrumbs::WorkKind dispatch_kind_ =
      VulkanGpuBreadcrumbs::WorkKind::kCount;
  uint32_t dispatch_detail_[VulkanGpuBreadcrumbs::kWorkDetailWords] = {};
  uint32_t dispatch_detail_count_ = 0;
  uint32_t large_dispatch_logs_ = 0;
  std::array<uint64_t, size_t(PassEndReason::kCount)> pass_ends_total_ = {};
  std::array<uint64_t, size_t(PassEndReason::kCount)> pass_ends_window_start_ =
      {};

  // A timestamp pair per in-flight submission, so the summed GPU busy time is
  // comparable against the Metal backend's completion-handler figure.
  static constexpr uint32_t kGpuTimeQuerySubmissions = 8;
  VkQueryPool gpu_time_query_pool_ = VK_NULL_HANDLE;
  uint64_t gpu_time_query_submissions_[kGpuTimeQuerySubmissions] = {};
  uint32_t gpu_time_query_slot_ = 0;
  uint64_t gpu_time_total_ns_ = 0;
  // Submissions that found every query slot busy, so their GPU time is missing
  // from the total.
  uint64_t gpu_time_untimed_submissions_ = 0;
  // Window start for the submission count published beside the busy time,
  // which sums per-submission intervals and misleads without it.
  uint64_t gpu_time_submissions_window_start_ = 0;
  uint64_t gpu_time_window_start_ns_ = 0;
  void CreateGpuTimeQueryPool();
  void DestroyGpuTimeQueryPool();
  void CollectCompletedGpuTimeQueries();

  // XR-3 pass profiler (xdroid_pass_profiler = N): bottom-of-pipe timestamps
  // before every render pass begin and after its end, for one frame in N, so
  // GPU time splits into per-pass and between-pass (dispatch/copy) time.
  enum class PassProfKind : uint8_t { kGuest, kTransfer, kClear, kCount };
  struct PassProfRecord {
    uint64_t submission;
    uint32_t query;  // Begin timestamp; the end one is query + 1.
    uint32_t draws;
    uint32_t width;
    uint32_t height;
    PassProfKind kind;
    PassEndReason end_reason;
    // GE-0: for a kBarrier end, the call site of the first barrier pushed
    // since the previous submit (PassProfBarrierSite), else 0.
    uint32_t barrier_site;
  };
  enum class PassProfState : uint8_t { kIdle, kRecording, kWaiting };
  static constexpr uint32_t kPassProfMaxPasses = 512;
  VkQueryPool pass_prof_pool_ = VK_NULL_HANDLE;
  bool pass_prof_pool_failed_ = false;
  PassProfState pass_prof_state_ = PassProfState::kIdle;
  uint64_t pass_prof_frame_ = 0;
  uint64_t pass_prof_last_submission_ = 0;
  // Whether the open render pass is being timed (its record is the last one).
  bool pass_prof_in_pass_ = false;
  std::vector<PassProfRecord> pass_prof_records_;
  // GE-0: call site of the first barrier pending since the last
  // SubmitBarriers while a frame is being profiled, and the site that ended
  // the open pass through SubmitBarriers.
  uint32_t pass_prof_pending_site_ = 0;
  uint32_t pass_prof_end_site_ = 0;
  void PassProfNoteBarrierSite(uint32_t line, const char* file);
  void PassProfBeginPass(PassProfKind kind, const VkExtent2D& extent);
  void PassProfEndPass(PassEndReason end_reason, const VkRect2D* patched_area);
  void PassProfCollect();
  void DestroyPassProfPool();
  // GE-2: the open pass has a render area smaller than its framebuffer, so it
  // can't be reused by anything that may write outside that area.
  bool pass_area_restricted_ = false;

  // GE-4 pass splice state for the open guest pass.
  bool splice_segment_open_ = false;
  size_t splice_segment_placeholder_ = 0;
  uint32_t splice_depth_ = 0;
  uint64_t draw_serial_ = 1;
  uint64_t pass_first_draw_serial_ = 1;
  std::array<const void*, 1 + xenos::kMaxColorRenderTargets> splice_pass_rts_ =
      {};
  std::vector<VkImage> splice_images_;
  // Spliced operations (texture loads, uploads, resolves, Enter-time barrier
  // batches), for the frame stats line.
  std::array<uint64_t, 4> splice_ops_total_ = {};
  std::array<uint64_t, 4> splice_ops_window_start_ = {};
  void OpenSpliceSegment(
      const VulkanRenderTargetCache::Framebuffer* framebuffer);
  void CloseSpliceSegment();
  // Ends a recording that is still open (only an aborted one is), keeping the
  // segment open for later work.
  void CloseSpliceRecordingIfAny() {
    if (splice_depth_) {
      splice_depth_ = 0;
      deferred_command_buffer_.EndSegment();
    }
  }
  // Whether every pending barrier may run before the open pass: buffers the
  // spliced work owns (shared memory, scratch) and images it wrote.
  bool ArePendingBarriersSpliceable() const;
  // Moves the pending barriers into the segment when the pass is about to be
  // reused, so they don't end it.
  bool TrySubmitPendingBarriersIntoSegment();
  void RecordPendingBarriers();
  // Guest frame index, since some transient resources can be reused across
  // submissions. Values updated in the beginning of a frame.
  uint64_t frame_current_ = 1;
  uint64_t frame_completed_ = 0;
  // Submission indices of frames that have already been submitted.
  uint64_t closed_frame_submissions_[kMaxFramesInFlight] = {};

  // <Submission where last used, resource>, sorted by the submission number.
  std::deque<std::pair<uint64_t, VkDeviceMemory>> destroy_memory_;
  std::deque<std::pair<uint64_t, VkBuffer>> destroy_buffers_;
  std::deque<std::pair<uint64_t, VkImage>> destroy_images_;
  std::deque<std::pair<uint64_t, VkImageView>> destroy_image_views_;
  std::deque<std::pair<uint64_t, VkFramebuffer>> destroy_framebuffers_;

  std::vector<CommandBuffer> command_buffers_writable_;
  std::deque<std::pair<uint64_t, CommandBuffer>> command_buffers_submitted_;
  DeferredCommandBuffer deferred_command_buffer_;

  std::vector<VkSparseMemoryBind> sparse_memory_binds_;
  std::vector<SparseBufferBind> sparse_buffer_binds_;
  // SparseBufferBind converted to VkSparseBufferMemoryBindInfo to this buffer
  // on submission (because pBinds should point to a place in std::vector, but
  // it may be reallocated).
  std::vector<VkSparseBufferMemoryBindInfo> sparse_buffer_bind_infos_temp_;
  VkPipelineStageFlags sparse_bind_wait_stage_mask_ = 0;

  // Temporary storage with reusable memory for creating descriptor set layouts.
  std::vector<VkDescriptorSetLayoutBinding> descriptor_set_layout_bindings_;
  // Temporary storage with reusable memory for writing image and sampler
  // descriptors.
  std::vector<VkDescriptorImageInfo> descriptor_write_image_info_;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> uniform_buffer_pool_;

  // Descriptor set layouts used by different shaders.
  VkDescriptorSetLayout descriptor_set_layout_empty_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout,
             size_t(SingleTransientDescriptorLayout::kCount)>
      descriptor_set_layouts_single_transient_{};
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_and_edram_ =
      VK_NULL_HANDLE;

  // Guards the layout maps below - GetPipelineLayout is called both from the
  // draw thread and from pipeline creation threads (for deferred translation).
  std::mutex pipeline_layouts_mutex_;
  // Descriptor set layouts are referenced by pipeline_layouts_.
  std::unordered_map<TextureDescriptorSetLayoutKey, VkDescriptorSetLayout,
                     TextureDescriptorSetLayoutKey::Hasher>
      descriptor_set_layouts_textures_;
  // Pipeline layouts are referenced by VulkanPipelineCache.
  std::unordered_map<PipelineLayoutKey, PipelineLayout,
                     PipelineLayoutKey::Hasher>
      pipeline_layouts_;

  // No specific reason for 32768, just the "too much" descriptor count from
  // Direct3D 12 PIX warnings.
  static constexpr uint32_t kLinkedTypeDescriptorPoolSetCount = 32768;
  static const VkDescriptorPoolSize kDescriptorPoolSizeUniformBufferDynamic;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageBuffer;
  static const VkDescriptorPoolSize kDescriptorPoolSizeStorageBufferAndImage[2];
  static const VkDescriptorPoolSize kDescriptorPoolSizeTextures[2];
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_uniform_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_storage_buffer_;
  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_storage_buffer_and_image_;
  std::deque<UsedSingleTransientDescriptor> single_transient_descriptors_used_;
  std::array<std::vector<VkDescriptorSet>,
             size_t(SingleTransientDescriptorLayout::kCount)>
      single_transient_descriptors_free_;
  // <Usage frame, set>.
  std::deque<std::pair<uint64_t, VkDescriptorSet>>
      constants_transient_descriptors_used_;
  std::vector<VkDescriptorSet> constants_transient_descriptors_free_;

  ui::vulkan::LinkedTypeDescriptorSetAllocator
      transient_descriptor_allocator_textures_;
  std::deque<UsedTextureTransientDescriptorSet>
      texture_transient_descriptor_sets_used_;
  std::unordered_map<TextureDescriptorSetLayoutKey,
                     std::vector<VkDescriptorSet>,
                     TextureDescriptorSetLayoutKey::Hasher>
      texture_transient_descriptor_sets_free_;

  std::unique_ptr<VulkanSharedMemory> shared_memory_;

  std::unique_ptr<VulkanPrimitiveProcessor> primitive_processor_;

  std::unique_ptr<VulkanRenderTargetCache> render_target_cache_;

  std::unique_ptr<VulkanZPDQueryPool> zpd_host_query_pool_;

  std::unique_ptr<VulkanPipelineCache> pipeline_cache_;

  std::unique_ptr<VulkanTextureCache> texture_cache_;

  VkDescriptorPool shared_memory_and_edram_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_and_edram_descriptor_set_;
  // Second set identical to the one above except binding 0 points at the
  // host-imported shared-memory buffer (aliasing guest RAM). Bound instead of
  // the device set for memexport-touching draws so their GPU-written geometry
  // stays coherent with the CPU. VK_NULL_HANDLE when the host buffer is
  // unavailable - routing is then disabled and every draw uses the device set.
  VkDescriptorSet shared_memory_host_and_edram_descriptor_set_ = VK_NULL_HANDLE;
  // Two-buffer memexport page tracking (data + methods), shared with the D3D12
  // backend as a class-body fragment so each backend gets its own non-virtual,
  // inlinable copy. Requires <array> and SharedMemory, both included above.
#include "../command_processor_memexport.inc"

  // Bytes 0x0...0x3FF - 256-entry gamma ramp table with B10G10R10X2 data (read
  // as R10G10B10X2 with swizzle).
  // Bytes 0x400...0x9FF - 128-entry PWL R16G16 gamma ramp (R - base, G - delta,
  // low 6 bits of each are zero, 3 elements per entry).
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  VkDeviceMemory gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
  // Display tone strength the uploaded gamma ramps were built with.
  float gamma_ramp_tone_strength_ = 0.0f;
  // Host-coherent word overwritten with each submission's index at its end,
  // for VulkanGPUCompletionTimeline::SetCompletionHint.
  VkBuffer completion_hint_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory completion_hint_memory_ = VK_NULL_HANDLE;
  volatile uint32_t* completion_hint_mapping_ = nullptr;
  VkBuffer gamma_ramp_buffer_ = VK_NULL_HANDLE;
  // kMaxFramesInFlight pairs, only when the gamma ramp buffer is not
  // host-visible.
  VkDeviceMemory gamma_ramp_upload_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer gamma_ramp_upload_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize gamma_ramp_upload_memory_size_;
  uint32_t gamma_ramp_upload_memory_type_;
  // Mapping of either gamma_ramp_buffer_memory_ (if it's host-visible) or
  // gamma_ramp_upload_buffer_memory_ (otherwise).
  void* gamma_ramp_upload_mapping_;
  std::array<VkBufferView, 2 * kMaxFramesInFlight> gamma_ramp_buffer_views_{};
  // UINT32_MAX if outdated.
  uint32_t gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  uint32_t gamma_ramp_pwl_current_frame_ = UINT32_MAX;

  VkDescriptorSetLayout swap_descriptor_set_layout_sampled_image_ =
      VK_NULL_HANDLE;
  VkDescriptorSetLayout swap_descriptor_set_layout_uniform_texel_buffer_ =
      VK_NULL_HANDLE;
  // Storage image descriptor set layout for compute shader destination.
  VkDescriptorSetLayout swap_descriptor_set_layout_storage_image_ =
      VK_NULL_HANDLE;

  // Descriptor pool for allocating descriptors needed for presentation, such as
  // the destination images and the gamma ramps.
  VkDescriptorPool swap_descriptor_pool_ = VK_NULL_HANDLE;
  // Interleaved 256-entry table and PWL texel buffer descriptors.
  // kMaxFramesInFlight pairs of gamma ramps if in host-visible memory and
  // uploaded directly, one otherwise.
  std::array<VkDescriptorSet, 2 * kMaxFramesInFlight>
      swap_descriptors_gamma_ramp_;
  // Sampled images for source.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_source_;
  // Storage images for destination.
  std::array<VkDescriptorSet, kMaxFramesInFlight> swap_descriptors_dest_;

  // Gamma ramp application compute pipeline constants.
  struct ApplyGammaConstants {
    uint32_t size[2];
  };
  VkPipelineLayout swap_apply_gamma_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_256_entry_table_pipeline_ = VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_pipeline_ = VK_NULL_HANDLE;
  // Gamma pipelines that also compute FXAA luma in the alpha channel.
  VkPipeline swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_ =
      VK_NULL_HANDLE;
  VkPipeline swap_apply_gamma_pwl_fxaa_luma_pipeline_ = VK_NULL_HANDLE;

  // FXAA post-processing.
  struct FxaaConstants {
    uint32_t size[2];
    float size_inv[2];
  };
  // Descriptor set layout for FXAA source (combined image sampler).
  VkDescriptorSetLayout fxaa_source_descriptor_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout fxaa_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline fxaa_pipeline_ = VK_NULL_HANDLE;
  VkPipeline fxaa_extreme_pipeline_ = VK_NULL_HANDLE;
  VkSampler fxaa_sampler_ = VK_NULL_HANDLE;

  // Resolve downscale compute shader for scaled resolution readback.
  // Downscales scaled resolve buffer data back to 1x resolution on the GPU,
  // avoiding expensive CPU-side downscaling and reducing transfer bandwidth.
  VkDescriptorSetLayout resolve_downscale_descriptor_set_layout_ =
      VK_NULL_HANDLE;
  VkPipelineLayout resolve_downscale_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline resolve_downscale_pipeline_ = VK_NULL_HANDLE;
  // Descriptor pool chain for resolve downscale shader (2 storage buffers per
  // set). Uses pool chain to avoid mid-frame GPU stalls on pool exhaustion.
  std::unique_ptr<ui::vulkan::VulkanDescriptorPoolChain>
      resolve_downscale_descriptor_pool_chain_;
  // Intermediate buffer for downscaled output (device local, for compute).
  VkBuffer resolve_downscale_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
  uint32_t resolve_downscale_buffer_size_ = 0;

  // FXAA source texture - R16G16B16A16_SFLOAT for luma in alpha.
  static constexpr VkFormat kFxaaSourceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  VkDeviceMemory fxaa_source_memory_ = VK_NULL_HANDLE;
  VkImage fxaa_source_image_ = VK_NULL_HANDLE;
  VkImageView fxaa_source_image_view_ = VK_NULL_HANDLE;
  uint32_t fxaa_source_width_ = 0;
  uint32_t fxaa_source_height_ = 0;
  uint64_t fxaa_source_last_submission_ = 0;
  // Descriptor sets for FXAA - source as combined image sampler (for FXAA
  // read).
  std::array<VkDescriptorSet, kMaxFramesInFlight> fxaa_source_descriptors_;
  // Descriptor sets for FXAA - source as storage image (for gamma+luma write).
  std::array<VkDescriptorSet, kMaxFramesInFlight>
      fxaa_source_storage_descriptors_;

  // Pending pipeline barriers.
  std::vector<VkBufferMemoryBarrier> pending_barriers_buffer_memory_barriers_;
  std::vector<VkImageMemoryBarrier> pending_barriers_image_memory_barriers_;
  struct PendingBarrier {
    VkPipelineStageFlags src_stage_mask = 0;
    VkPipelineStageFlags dst_stage_mask = 0;
    size_t buffer_memory_barriers_offset = 0;
    size_t image_memory_barriers_offset = 0;
  };
  std::vector<PendingBarrier> pending_barriers_;
  PendingBarrier current_pending_barrier_;

  // GPU-local scratch buffer.
  static constexpr VkDeviceSize kScratchBufferSizeIncrement = 16 * 1024 * 1024;
  VkDeviceMemory scratch_buffer_memory_ = VK_NULL_HANDLE;
  VkBuffer scratch_buffer_ = VK_NULL_HANDLE;
  VkDeviceSize scratch_buffer_size_ = 0;
  VkPipelineStageFlags scratch_buffer_last_stage_mask_ = 0;
  VkAccessFlags scratch_buffer_last_access_mask_ = 0;
  uint64_t scratch_buffer_last_usage_submission_ = 0;
  bool scratch_buffer_used_ = false;

  // The current dynamic state of the graphics pipeline bind point. Note that
  // binding any pipeline to the bind point with static state (even if it's
  // unused, like depth bias being disabled, but the values themselves still not
  // declared as dynamic in the pipeline) invalidates such dynamic state.
  VkViewport dynamic_viewport_;
  VkRect2D dynamic_scissor_;
  // Dynamic fixed-function depth bias, blend constants, stencil state are
  // applicable only to the render target implementations where they are
  // actually involved.
  float dynamic_depth_bias_constant_factor_;
  float dynamic_depth_bias_slope_factor_;
  float dynamic_blend_constants_[4];
  // The stencil values are pre-initialized (to D3D11_DEFAULT_STENCIL_*, and the
  // initial values for front and back are the same for portability subset
  // safety) because they're updated conditionally to avoid changing the back
  // face values when stencil is disabled and the primitive type is changed
  // between polygonal and non-polygonal.
  uint32_t dynamic_stencil_compare_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_compare_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_front_ = UINT8_MAX;
  uint32_t dynamic_stencil_write_mask_back_ = UINT8_MAX;
  uint32_t dynamic_stencil_reference_front_ = 0;
  uint32_t dynamic_stencil_reference_back_ = 0;
  bool dynamic_viewport_update_needed_;
  bool dynamic_scissor_update_needed_;
  bool dynamic_depth_bias_update_needed_;
  bool dynamic_blend_constants_update_needed_;
  bool dynamic_stencil_compare_mask_front_update_needed_;
  bool dynamic_stencil_compare_mask_back_update_needed_;
  bool dynamic_stencil_write_mask_front_update_needed_;
  bool dynamic_stencil_write_mask_back_update_needed_;
  bool dynamic_stencil_reference_front_update_needed_;
  bool dynamic_stencil_reference_back_update_needed_;

  // Currently used samplers.
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_samplers_vertex_;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_samplers_pixel_;

  // Cache render pass currently started in the command buffer with the
  // framebuffer. For dynamic rendering, current_render_pass_ is VK_NULL_HANDLE
  // but in_render_pass_ is true.
  VkRenderPass current_render_pass_;
  // SHD-1 (xdroid_shader_stats): draws and indices per (VS, PS) hash pair.
  struct ShaderDrawStat {
    uint64_t draws = 0;
    uint64_t indices = 0;
  };
  std::map<std::pair<uint64_t, uint64_t>, ShaderDrawStat> shader_draw_stats_;
  const VulkanRenderTargetCache::Framebuffer* current_framebuffer_;
  // True when inside a render pass or dynamic rendering block.
  bool in_render_pass_ = false;

  // Currently bound graphics pipeline, either from the pipeline cache (with
  // potentially deferred creation - current_external_graphics_pipeline_ is
  // VK_NULL_HANDLE in this case) or a non-Xenos one
  // (current_guest_graphics_pipeline_ is VK_NULL_HANDLE in this case).
  // TODO(Triang3l): Change to a deferred compilation handle.
  VkPipeline current_guest_graphics_pipeline_;
  VkPipeline current_external_graphics_pipeline_;
  VkPipeline current_external_compute_pipeline_;

  // Pipeline layout of the current guest graphics pipeline.
  const PipelineLayout* current_guest_graphics_pipeline_layout_;
  // The bindings are VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, so the infos
  // describe only the pool page and the size - the offset within the page is
  // passed when binding and the info offset stays 0.
  VkDescriptorBufferInfo current_constant_buffer_infos_
      [SpirvShaderTranslator::kConstantBufferCount]{};
  uint32_t current_constant_buffer_dynamic_offsets_
      [SpirvShaderTranslator::kConstantBufferCount]{};
  // Whether up-to-date data has been written to constant (uniform) buffers, and
  // the buffer infos in current_constant_buffer_infos_ point to them.
  uint32_t current_constant_buffers_up_to_date_;
  VkDescriptorSet current_graphics_descriptor_sets_
      [SpirvShaderTranslator::kDescriptorSetCount];
  // Whether descriptor sets in current_graphics_descriptor_sets_ point to
  // up-to-date data.
  uint32_t current_graphics_descriptor_set_values_up_to_date_;
  // Whether the descriptor sets currently bound to the command buffer - only
  // low bits for the descriptor set layouts that remained the same are kept
  // when changing the pipeline layout. May be out of sync with
  // current_graphics_descriptor_set_values_up_to_date_, but should be ensured
  // to be a subset of it at some point when it becomes important; bits for
  // non-existent descriptor set layouts may also be set, but need to be ignored
  // when they start to matter.
  uint32_t current_graphics_descriptor_sets_bound_up_to_date_;
  static_assert(
      SpirvShaderTranslator::kDescriptorSetCount <=
          sizeof(current_graphics_descriptor_set_values_up_to_date_) * CHAR_BIT,
      "Bit fields storing descriptor set validity must be large enough");
  static_assert(
      SpirvShaderTranslator::kDescriptorSetCount <=
          sizeof(current_graphics_descriptor_sets_bound_up_to_date_) * CHAR_BIT,
      "Bit fields storing descriptor set validity must be large enough");
  // For reusing the texture/sampler descriptor sets across draws while their
  // contents stay valid - the shaders and the samplers the currently valid
  // texture descriptor sets were written for (texture image view changes are
  // tracked separately via VulkanTextureCache::texture_bindings_changed).
  const VulkanShader* current_textures_vertex_shader_ = nullptr;
  const VulkanShader* current_textures_pixel_shader_ = nullptr;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_written_samplers_vertex_;
  std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>
      current_written_samplers_pixel_;

  // Float constant usage masks of the last draw call.
  uint64_t current_float_constant_map_vertex_[4];
  uint64_t current_float_constant_map_pixel_[4];
  // True if the vertex float constant buffer holds the full unpacked 256 float
  // constants for the ucode interpreter rather than a shader's packed subset.
  // Used to invalidate the binding when switching interpreter <-> normal.
  bool float_constants_vertex_are_full_ = false;

  // System shader constants, including user clip planes and tessellation
  // constants.
  SpirvShaderTranslator::SystemConstants system_constants_;

  // Host viewport of the previous draw, reused while the inputs it was derived
  // from stay the same.
  draw_util::GetViewportInfoArgs previous_viewport_info_args_{};
  draw_util::ViewportInfo previous_viewport_info_{};

  // Temporary storage for memexport stream constants used in the draw.
  std::vector<draw_util::MemExportRange> memexport_ranges_;

  // Backend-agnostic resolve-to-guest-RAM copy decisions and read-watch
  // consumption tracking, shared with the D3D12 backend.
#include "../command_processor_resolve_readwatch.inc"
  // Per-backend trampoline from the memory read callback into the shared
  // MarkResolvePagesRead, PrepareResolvePagesForWrite or DiscardResolvePages.
  static void ResolveReadCallbackThunk(void* context, uint32_t physical_address,
                                       uint32_t length,
                                       Memory::PhysicalAccess access);
  // Downscaling a scaled resolve's output into resolve_downscale_buffer_ for a
  // copy out of it, and writing it into the shared memory buffer where a native
  // resolve would have, for a fault to copy it from there. Declared after the
  // .inc for ScaledResolveReadbackInfo.
  bool DownscaleScaledResolve(uint32_t written_address,
                              const ScaledResolveReadbackInfo& scaled_info);
  bool MirrorScaledResolveToSharedMemory(uint32_t written_address,
                                         uint32_t written_length,
                                         reg::RB_COPY_DEST_INFO copy_dest_info,
                                         uint32_t& mirrored_length_out);

  // Debug marker support for RenderDoc/debug tools.
  bool debug_markers_enabled_ = false;
  void UpdateDebugMarkersEnabled();
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_COMMAND_PROCESSOR_H_
