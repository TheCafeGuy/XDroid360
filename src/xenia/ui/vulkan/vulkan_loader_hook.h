/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_VULKAN_VULKAN_LOADER_HOOK_H_
#define XENIA_UI_VULKAN_VULKAN_LOADER_HOOK_H_

// Kept free of Vulkan headers so that embedders can include it without the
// Vulkan build setup.

namespace xe {
namespace ui {
namespace vulkan {

// Overrides how VulkanInstance opens the driver library, for example to load
// a user-supplied driver on Android. The opener returns a dlopen-style handle
// that exports vkGetInstanceProcAddr, or null to fall back to the system
// loader. Set before creating the instance; null restores the default.
using VulkanLoaderOpener = void* (*)();
void SetVulkanLoaderOpener(VulkanLoaderOpener opener);

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_VULKAN_LOADER_HOOK_H_
