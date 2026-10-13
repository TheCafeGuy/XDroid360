// XDroid360 user-selected Vulkan driver (for example Turnip), loaded through
// libadrenotools and handed to the upstream instance via
// VulkanInstance::SetLoaderOpener.
#ifndef XDROID_DRIVER_H_
#define XDROID_DRIVER_H_

#include <string>

namespace xdroid {

// Selects the driver for the next Vulkan instance. An empty driver path keeps
// the system driver. hook_dir is the app's native library dir, which holds
// the adrenotools hook libraries. tu_debug_file, if it exists and is not
// empty, sets TU_DEBUG for the driver (Turnip debug and workaround flags
// without a rebuild); without it TU_DEBUG defaults to noconcurrentresolves.
void SelectVulkanDriver(const std::string& driver_path,
                        const std::string& hook_dir,
                        const std::string& tu_debug_file);

}  // namespace xdroid

#endif  // XDROID_DRIVER_H_
