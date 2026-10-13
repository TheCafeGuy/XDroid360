/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_VULKAN_VULKAN_GPU_COMPLETION_TIMELINE_H_
#define XENIA_UI_VULKAN_VULKAN_GPU_COMPLETION_TIMELINE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/ui/gpu_completion_timeline.h"
#include "xenia/ui/vulkan/vulkan_device.h"

namespace xe {
namespace ui {
namespace vulkan {

class VulkanGPUCompletionTimeline : public GPUCompletionTimeline {
 public:
  explicit VulkanGPUCompletionTimeline(VulkanDevice* const vulkan_device,
                                       const char* const name = "")
      : vulkan_device_(vulkan_device), name_(name) {}

  const char* name() const { return name_; }
  size_t pending_submission_count() const {
    return pending_submission_fences_.size();
  }
  uint64_t front_pending_submission() const {
    return pending_submission_fences_.empty()
               ? 0
               : pending_submission_fences_.front().first;
  }

  VulkanGPUCompletionTimeline(const VulkanGPUCompletionTimeline&) = delete;
  VulkanGPUCompletionTimeline& operator=(const VulkanGPUCompletionTimeline&) =
      delete;
  VulkanGPUCompletionTimeline(VulkanGPUCompletionTimeline&&) = delete;
  VulkanGPUCompletionTimeline& operator=(VulkanGPUCompletionTimeline&&) =
      delete;

  ~VulkanGPUCompletionTimeline();

  class FenceAcquisition {
   public:
    explicit FenceAcquisition(
        VulkanGPUCompletionTimeline* const completion_timeline,
        const VkFence fence)
        : completion_timeline_(completion_timeline), fence_(fence) {
      assert_not_null(completion_timeline);
      assert_true(fence != VK_NULL_HANDLE);
    }

    FenceAcquisition(const FenceAcquisition&) = delete;
    FenceAcquisition& operator=(const FenceAcquisition&) = delete;

    FenceAcquisition(FenceAcquisition&& other)
        : completion_timeline_(other.completion_timeline_),
          fence_(other.fence_),
          submission_successful_(other.submission_successful_) {
      other.completion_timeline_ = nullptr;
      other.fence_ = VK_NULL_HANDLE;
      other.submission_successful_.reset();
    }

    ~FenceAcquisition() {
      if (completion_timeline_ && fence_) {
#ifndef NDEBUG
        assert_not_zero(completion_timeline_->fences_acquired_);
        --completion_timeline_->fences_acquired_;
#endif
        if (submission_successful_.value_or(false)) {
          assert_true(
              completion_timeline_->pending_submission_fences_.empty() ||
              completion_timeline_->pending_submission_fences_.front().first <
                  completion_timeline_->GetUpcomingSubmission());
          completion_timeline_->pending_submission_fences_.emplace_back(
              completion_timeline_->GetUpcomingSubmission(), fence_);
          completion_timeline_->OnFenceSubmitted(
              completion_timeline_->GetUpcomingSubmission(), fence_);
          completion_timeline_->IncrementUpcomingSubmission();
        } else {
          completion_timeline_->free_fences_.push_back(fence_);
        }
      }
    }

    VkFence GetFenceForSubmitting() {
      // Don't mark the fence as used in a submission if already tried to
      // submit, but failed.
      assert_true(!submission_successful_.has_value() ||
                  *submission_successful_);
      submission_successful_ = true;
      return fence_;
    }

    void SetSubmissionFailedOrAborted() { submission_successful_ = false; }

   private:
    VulkanGPUCompletionTimeline* completion_timeline_ = nullptr;

    VkFence fence_ = VK_NULL_HANDLE;

    std::optional<bool> submission_successful_;
  };

  // If the submission has succeeded (`GetFenceForSubmitting` was called, but
  // `SetSubmissionFailedOrAborted` was not), will advance to the next
  // submission once the acquisition is released.
  //
  // It's possible to acquire a fence not right before submitting, but also well
  // in advance, such as before recording the command buffer, for instance, to
  // skip recording it if fence acquisition has failed.
  //
  // Acquiring a fence also updates the completed submission in order to reuse
  // fences if this completion timeline is used without regular checks or waits
  // (for instance, if it's supplementary to another completion timeline, and
  // awaited only before destroying something).
  [[nodiscard]] std::optional<FenceAcquisition> AcquireFenceForSubmission(
      VkResult* result_out_opt = nullptr);

  VkResult AcquireFenceAndSubmit(uint32_t queue_family_index,
                                 uint32_t queue_index, uint32_t submit_count,
                                 const VkSubmitInfo* submits);

  void UpdateCompletedSubmission() override;

  // XDroid360: waits up to timeout_ms for a submission without updating the
  // completed submission. False only on timeout; a completed submission or a
  // failed wait returns true (the regular await reports failures).
  bool WaitSubmissionFor(uint64_t awaited_submission, uint32_t timeout_ms);

  // Optional host-visible, host-coherent word the GPU overwrites with the low
  // 32 bits of each submission index once everything before it in the queue
  // has executed. When set, fences are queried only for submissions it already
  // reports as done, so polling costs a memory load instead of a fence status
  // query (which sleeps in the kernel on Turnip / kgsl). The fence remains the
  // authority: a premature value only costs one query.
  void SetCompletionHint(const volatile uint32_t* hint) {
    completion_hint_ = hint;
  }

 protected:
  void AwaitSubmissionImpl(uint64_t awaited_submission) override;

 private:
  VulkanDevice* const vulkan_device_;
  const char* const name_;

  std::vector<VkFence> free_fences_;

  // <Submission index, fence>, in submission index order.
  std::deque<std::pair<uint64_t, VkFence>> pending_submission_fences_;

#ifndef NDEBUG
  size_t fences_acquired_ = 0;
#endif

  // Optional fence waiter thread (cvar vulkan_async_fence_waiter). On drivers
  // where vkGetFenceStatus sleeps in the kernel (Turnip on kgsl), polling the
  // fences from the submitting thread blocks it on every check. The waiter
  // blocks on the fences in submission order instead and publishes the last
  // completed submission, so checks become an atomic load.
  void OnFenceSubmitted(uint64_t submission, VkFence fence);
  void WaiterMain();
  void StopWaiter();
  bool waiter_decided_ = false;
  bool waiter_enabled_ = false;
  bool waiter_lost_reported_ = false;
  std::thread waiter_thread_;
  std::mutex waiter_mutex_;
  std::condition_variable waiter_cv_;
  std::condition_variable completed_cv_;
  // Submitted fences not yet awaited by the waiter, in submission order.
  std::deque<std::pair<uint64_t, VkFence>> waiter_queue_;
  std::atomic<uint64_t> waiter_completed_{0};
  std::atomic<bool> waiter_lost_{false};
  std::atomic<bool> waiter_stop_{false};
  std::chrono::steady_clock::time_point last_poll_time_{};
  const volatile uint32_t* completion_hint_ = nullptr;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_VULKAN_GPU_COMPLETION_TIMELINE_H_
