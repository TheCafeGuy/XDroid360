/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/vulkan/vulkan_gpu_completion_timeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

#if defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#endif

DEFINE_uint32(vulkan_fence_poll_interval_ms, 4,
              "Without vulkan_async_fence_waiter, poll submission fences at "
              "most this often (ms). 0 = on every check.",
              "Vulkan");
DEFINE_bool(
    vulkan_async_fence_waiter, false,
    "Wait on Vulkan submission fences on a dedicated thread instead of polling "
    "them with vkGetFenceStatus on the submitting thread. Helps drivers where "
    "a fence status query sleeps in the kernel (Turnip on kgsl).",
    "Vulkan");

namespace xe {
namespace ui {
namespace vulkan {

VulkanGPUCompletionTimeline::~VulkanGPUCompletionTimeline() {
  StopWaiter();

#ifndef NDEBUG
  assert_zero(fences_acquired_);
#endif

  if (!pending_submission_fences_.empty()) {
    if (vulkan_device_->functions().vkWaitForFences(
            vulkan_device_->device(), 1,
            &pending_submission_fences_.back().second, VK_TRUE,
            UINT64_MAX) == VK_ERROR_DEVICE_LOST) {
      XELOGE(
          "VulkanGPUCompletionTimeline[{}]: DEVICE_LOST waiting on final "
          "submission {} during destruction ({} fences pending)",
          name_, pending_submission_fences_.back().first,
          pending_submission_fences_.size());
      if (vulkan_device_->SetLost()) {
        vulkan_device_->LogFaultInfo();
      }
    }

    while (!pending_submission_fences_.empty()) {
      vulkan_device_->functions().vkDestroyFence(
          vulkan_device_->device(), pending_submission_fences_.back().second,
          nullptr);
      pending_submission_fences_.pop_back();
    }
  }

  while (!free_fences_.empty()) {
    vulkan_device_->functions().vkDestroyFence(vulkan_device_->device(),
                                               free_fences_.back(), nullptr);
    free_fences_.pop_back();
  }
}

std::optional<VulkanGPUCompletionTimeline::FenceAcquisition>
VulkanGPUCompletionTimeline::AcquireFenceForSubmission(
    VkResult* const result_out_opt) {
  // Reuse fences if completion was not awaited or updated explicitly.
  UpdateAndGetCompletedSubmission();

  VkFence fence = VK_NULL_HANDLE;

  if (!free_fences_.empty()) {
    const VkResult fence_reset_result =
        vulkan_device_->functions().vkResetFences(vulkan_device_->device(), 1,
                                                  &free_fences_.back());
    if (fence_reset_result != VK_SUCCESS) {
      XELOGE("Failed to reset a Vulkan fence: {}",
             vk::to_string(vk::Result(fence_reset_result)));
    } else {
      fence = free_fences_.back();
      free_fences_.pop_back();
    }
  }

  if (fence == VK_NULL_HANDLE) {
    const VkFenceCreateInfo fence_create_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const VkResult fence_create_result =
        vulkan_device_->functions().vkCreateFence(
            vulkan_device_->device(), &fence_create_info, nullptr, &fence);
    if (fence_create_result != VK_SUCCESS) {
      XELOGE("Failed to create a Vulkan fence: {}",
             vk::to_string(vk::Result(fence_create_result)));
      if (result_out_opt != nullptr) {
        *result_out_opt = fence_create_result;
      }
      return std::nullopt;
    }
  }

#ifndef NDEBUG
  ++fences_acquired_;
#endif

  if (result_out_opt != nullptr) {
    *result_out_opt = VK_SUCCESS;
  }
  return FenceAcquisition(this, fence);
}

VkResult VulkanGPUCompletionTimeline::AcquireFenceAndSubmit(
    const uint32_t queue_family_index, const uint32_t queue_index,
    const uint32_t submit_count, const VkSubmitInfo* const submits) {
  VkResult fence_acquire_result;
  std::optional<FenceAcquisition> fence_acquisition =
      AcquireFenceForSubmission(&fence_acquire_result);
  if (!fence_acquisition.has_value()) {
    return fence_acquire_result;
  }

  VkResult submit_result;
  {
    const VulkanDevice::Queue::Acquisition queue_acquisition =
        vulkan_device_->AcquireQueue(queue_family_index, queue_index);
    submit_result = vulkan_device_->SubmitAndUpdateLost(
        queue_acquisition.queue(), submit_count, submits,
        fence_acquisition->GetFenceForSubmitting());
  }
  if (submit_result != VK_SUCCESS) {
    fence_acquisition->SetSubmissionFailedOrAborted();
  }
  return submit_result;
}

void VulkanGPUCompletionTimeline::UpdateCompletedSubmission() {
  if (waiter_enabled_) {
    if (waiter_lost_.load(std::memory_order_acquire)) {
      if (!waiter_lost_reported_) {
        waiter_lost_reported_ = true;
        if (vulkan_device_->SetLost()) {
          vulkan_device_->LogFaultInfo();
        }
      }
      return;
    }
    const uint64_t done = waiter_completed_.load(std::memory_order_acquire);
    while (!pending_submission_fences_.empty() &&
           pending_submission_fences_.front().first <= done) {
      SetCompletedSubmission(pending_submission_fences_.front().first);
      free_fences_.push_back(pending_submission_fences_.front().second);
      pending_submission_fences_.pop_front();
    }
    return;
  }
  if (completion_hint_) {
    const uint32_t hint = *completion_hint_;
    std::atomic_thread_fence(std::memory_order_acquire);
    while (!pending_submission_fences_.empty() &&
           int32_t(hint - uint32_t(pending_submission_fences_.front().first)) >=
               0) {
      const VkResult fence_status =
          vulkan_device_->functions().vkGetFenceStatus(
              vulkan_device_->device(),
              pending_submission_fences_.front().second);
      if (fence_status != VK_SUCCESS) {
        if (fence_status == VK_ERROR_DEVICE_LOST) {
          XELOGE(
              "VulkanGPUCompletionTimeline[{}]: DEVICE_LOST polling fence for "
              "submission {} ({} fences pending)",
              name_, pending_submission_fences_.front().first,
              pending_submission_fences_.size());
          if (vulkan_device_->SetLost()) {
            vulkan_device_->LogFaultInfo();
          }
        }
        break;
      }
      SetCompletedSubmission(pending_submission_fences_.front().first);
      free_fences_.push_back(pending_submission_fences_.front().second);
      pending_submission_fences_.pop_front();
    }
    return;
  }
  // Single-threaded alternative to the waiter: poll at most every
  // vulkan_fence_poll_interval_ms, so the submitting thread rarely enters the
  // (sleeping) fence status query. Blocking awaits are unaffected.
  if (cvars::vulkan_fence_poll_interval_ms) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_poll_time_ <
        std::chrono::milliseconds(cvars::vulkan_fence_poll_interval_ms)) {
      return;
    }
    last_poll_time_ = now;
  }
  while (!pending_submission_fences_.empty()) {
    const VkResult fence_status = vulkan_device_->functions().vkGetFenceStatus(
        vulkan_device_->device(), pending_submission_fences_.front().second);
    if (fence_status != VK_SUCCESS) {
      // Not ready, or an error.
      if (fence_status == VK_ERROR_DEVICE_LOST) {
        XELOGE(
            "VulkanGPUCompletionTimeline[{}]: DEVICE_LOST polling fence for "
            "submission {} ({} fences pending)",
            name_, pending_submission_fences_.front().first,
            pending_submission_fences_.size());
        if (vulkan_device_->SetLost()) {
          vulkan_device_->LogFaultInfo();
        }
      }
      break;
    }
    SetCompletedSubmission(pending_submission_fences_.front().first);
    free_fences_.push_back(pending_submission_fences_.front().second);
    pending_submission_fences_.pop_front();
  }
}

void VulkanGPUCompletionTimeline::OnFenceSubmitted(const uint64_t submission,
                                                   const VkFence fence) {
  if (!waiter_decided_) {
    waiter_decided_ = true;
    waiter_enabled_ = cvars::vulkan_async_fence_waiter;
    if (waiter_enabled_) {
      waiter_thread_ = std::thread([this]() { WaiterMain(); });
    }
  }
  if (!waiter_enabled_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(waiter_mutex_);
    waiter_queue_.emplace_back(submission, fence);
  }
  waiter_cv_.notify_one();
}

void VulkanGPUCompletionTimeline::WaiterMain() {
#if defined(__linux__) || defined(__ANDROID__)
  pthread_setname_np(pthread_self(), "VkFenceWaiter");
#endif
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  std::unique_lock<std::mutex> lock(waiter_mutex_);
  while (true) {
    waiter_cv_.wait(lock, [this]() {
      return waiter_stop_.load(std::memory_order_relaxed) ||
             !waiter_queue_.empty();
    });
    if (waiter_stop_.load(std::memory_order_relaxed)) {
      break;
    }
    const std::pair<uint64_t, VkFence> front = waiter_queue_.front();
    lock.unlock();
    VkResult result;
    do {
      // Bounded so that a stop request is noticed.
      result = dfn.vkWaitForFences(device, 1, &front.second, VK_TRUE,
                                   UINT64_C(50000000));
    } while (result == VK_TIMEOUT &&
             !waiter_stop_.load(std::memory_order_relaxed));
    lock.lock();
    if (result == VK_SUCCESS) {
      waiter_queue_.pop_front();
      waiter_completed_.store(front.first, std::memory_order_release);
    } else if (result != VK_TIMEOUT) {
      XELOGE(
          "VulkanGPUCompletionTimeline[{}]: fence waiter vkWaitForFences -> {} "
          "(submission {})",
          name_, vk::to_string(vk::Result(result)), front.first);
      waiter_lost_.store(true, std::memory_order_release);
      completed_cv_.notify_all();
      waiter_cv_.wait(lock, [this]() {
        return waiter_stop_.load(std::memory_order_relaxed);
      });
      break;
    }
    completed_cv_.notify_all();
  }
}

void VulkanGPUCompletionTimeline::StopWaiter() {
  if (!waiter_thread_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(waiter_mutex_);
    waiter_stop_.store(true, std::memory_order_relaxed);
  }
  waiter_cv_.notify_all();
  waiter_thread_.join();
}

bool VulkanGPUCompletionTimeline::WaitSubmissionFor(
    const uint64_t awaited_submission, const uint32_t timeout_ms) {
  if (waiter_enabled_) {
    std::unique_lock<std::mutex> lock(waiter_mutex_);
    return completed_cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [this, awaited_submission]() {
          return waiter_completed_.load(std::memory_order_acquire) >=
                     awaited_submission ||
                 waiter_lost_.load(std::memory_order_acquire);
        });
  }
  VkFence fence = VK_NULL_HANDLE;
  for (const auto& pending : pending_submission_fences_) {
    if (pending.first > awaited_submission) {
      break;
    }
    fence = pending.second;
  }
  if (fence == VK_NULL_HANDLE) {
    return true;
  }
  return vulkan_device_->functions().vkWaitForFences(
             vulkan_device_->device(), 1, &fence, VK_TRUE,
             uint64_t(timeout_ms) * 1000000) != VK_TIMEOUT;
}

void VulkanGPUCompletionTimeline::AwaitSubmissionImpl(
    const uint64_t awaited_submission) {
  if (waiter_enabled_) {
    {
      std::unique_lock<std::mutex> lock(waiter_mutex_);
      completed_cv_.wait(lock, [this, awaited_submission]() {
        return waiter_completed_.load(std::memory_order_acquire) >=
                   awaited_submission ||
               waiter_lost_.load(std::memory_order_acquire);
      });
    }
    UpdateCompletedSubmission();
    return;
  }
  // According to the Vulkan 1.4.335 specification:
  // "The first synchronization scope includes every batch submitted in the same
  // queue submission command. Fence signal operations that are defined by
  // vkQueueSubmit or vkQueueSubmit2 additionally include in the first
  // synchronization scope all commands that occur earlier in submission order.
  // Fence signal operations that are defined by vkQueueSubmit or vkQueueSubmit2
  // or vkQueueBindSparse additionally include in the first synchronization
  // scope any semaphore and fence signal operations that occur earlier in
  // signal operation order."
  auto submission_end_iterator = pending_submission_fences_.cbegin();
  while (submission_end_iterator != pending_submission_fences_.cend() &&
         submission_end_iterator->first <= awaited_submission) {
    submission_end_iterator = std::next(submission_end_iterator);
  }
  if (submission_end_iterator != pending_submission_fences_.cbegin()) {
    const VkResult fence_wait_result =
        vulkan_device_->functions().vkWaitForFences(
            vulkan_device_->device(), 1,
            &std::prev(submission_end_iterator)->second, VK_TRUE, UINT64_MAX);
    if (fence_wait_result != VK_SUCCESS) {
      XELOGE(
          "VulkanGPUCompletionTimeline[{}]: vkWaitForFences -> {} (awaited "
          "submission {}, last-pending {}, {} fences pending)",
          name_, vk::to_string(vk::Result(fence_wait_result)),
          awaited_submission, std::prev(submission_end_iterator)->first,
          pending_submission_fences_.size());
      if (fence_wait_result == VK_ERROR_DEVICE_LOST) {
        if (vulkan_device_->SetLost()) {
          vulkan_device_->LogFaultInfo();
        }
      }
      return;
    }
    for (auto free_fence_iterator = pending_submission_fences_.cbegin();
         free_fence_iterator != submission_end_iterator;
         free_fence_iterator = std::next(free_fence_iterator)) {
      free_fences_.push_back(free_fence_iterator->second);
    }
    pending_submission_fences_.erase(pending_submission_fences_.cbegin(),
                                     submission_end_iterator);
  }
  if (GetCompletedSubmissionFromLastUpdate() < awaited_submission) {
    SetCompletedSubmission(awaited_submission);
  }
}

}  // namespace vulkan
}  // namespace ui
}  // namespace xe
