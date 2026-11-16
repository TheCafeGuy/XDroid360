/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_VULKAN_VULKAN_MEM_ALLOC_H_
#define XENIA_UI_VULKAN_VULKAN_MEM_ALLOC_H_

// Make sure vulkan.h is included from third_party (rather than from the system
// include directory) before vk_mem_alloc.h.

#include <atomic>
#include <cstdint>

#include "xenia/ui/vulkan/vulkan_device.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
// Work around the pointer nullability completeness warnings on Clang.
#ifndef VMA_NULLABLE
#define VMA_NULLABLE
#endif
#ifndef VMA_NOT_NULL
#define VMA_NOT_NULL
#endif
#include "third_party/VulkanMemoryAllocator/include/vk_mem_alloc.h"

namespace xe {
namespace ui {
namespace vulkan {

// XDroid360 XDROID-LAZY texmem: called after each vkAllocateMemory done by
// VMA, with its wall time and size. Any allocating thread; lock-free.
extern std::atomic<void (*)(uint64_t ns, uint64_t bytes)>
    xdroid_vma_allocate_note;

VmaAllocator CreateVmaAllocator(const VulkanDevice* vulkan_device,
                                bool externally_synchronized);

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_VULKAN_MEM_ALLOC_H_
