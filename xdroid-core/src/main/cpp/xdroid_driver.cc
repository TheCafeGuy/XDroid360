// XDroid360 user-selected Vulkan driver. See xdroid_driver.h.
#include "xdroid_driver.h"

#include <dlfcn.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "adrenotools/driver.h"
#include "xenia/base/logging.h"
#include "xenia/ui/vulkan/vulkan_loader_hook.h"

namespace xdroid {
namespace {

std::string g_driver_path;
std::string g_hook_dir;

void* OpenSelectedDriver() {
  const std::filesystem::path driver(g_driver_path);
  // adrenotools joins the directory and the file name as plain strings.
  const std::string dir = driver.parent_path().string() + "/";
  const std::string name = driver.filename().string();
  void* handle = adrenotools_open_libvulkan(
      RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, /*tmpLibDir=*/nullptr,
      g_hook_dir.c_str(), dir.c_str(), name.c_str(),
      /*fileRedirectDir=*/nullptr, /*userMappingHandle=*/nullptr);
  if (!handle) {
    XELOGE("XDroid360: adrenotools could not open {}", g_driver_path);
    return nullptr;
  }
  if (!dlsym(handle, "vkGetInstanceProcAddr")) {
    XELOGE("XDroid360: {} has no vkGetInstanceProcAddr", g_driver_path);
    dlclose(handle);
    return nullptr;
  }
  XELOGI("XDroid360: Vulkan driver {}", g_driver_path);
  return handle;
}

// First line: TU_DEBUG flags. Later lines may set other Mesa debug variables
// as NAME=value (only the names below), for driver-side A/B arms.
void ApplyTuDebugFile(const std::string& path) {
  std::ifstream file(path);
  std::string line;
  bool first = true;
  while (file && std::getline(file, line)) {
    while (!line.empty() && std::isspace(uint8_t(line.back()))) {
      line.pop_back();
    }
    const bool is_first = first;
    first = false;
    if (line.empty()) {
      continue;
    }
    if (is_first && line.find('=') == std::string::npos) {
      setenv("TU_DEBUG", line.c_str(), 1);
      XELOGI("XDroid360: TU_DEBUG={} (from {})", line, path);
      continue;
    }
    const size_t equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string name = line.substr(0, equals);
    if (name != "TU_DEBUG" && name != "IR3_SHADER_DEBUG" &&
        name != "FD_MESA_DEBUG" && name != "TU_AUTOTUNE_ALGO" &&
        name != "TU_AUTOTUNE_FLAGS") {
      XELOGW("XDroid360: {} ignores {}", path, name);
      continue;
    }
    setenv(name.c_str(), line.c_str() + equals + 1, 1);
    XELOGI("XDroid360: {} (from {})", line, path);
  }
}

}  // namespace

void SelectVulkanDriver(const std::string& driver_path,
                        const std::string& hook_dir,
                        const std::string& tu_debug_file) {
  if (!tu_debug_file.empty()) {
    ApplyTuDebugFile(tu_debug_file);
  }
  // Turnip a7xx concurrent resolves hang the GPU when a compute pass reads a
  // 2x MSAA depth attachment right after its render pass (STAB-S1: 0 hangs in
  // 2 full missions with the flag). Only Turnip reads TU_DEBUG and only a7xx
  // has concurrent resolves, so other GPUs are unaffected. tu_debug.txt
  // overrides it; "TU_DEBUG=" there turns it off.
  if (!getenv("TU_DEBUG")) {
    setenv("TU_DEBUG", "noconcurrentresolves", 1);
    XELOGI("XDroid360: TU_DEBUG=noconcurrentresolves (default)");
  }
  g_driver_path = driver_path;
  g_hook_dir = hook_dir;
  const bool custom = !g_driver_path.empty() && !g_hook_dir.empty();
  xe::ui::vulkan::SetVulkanLoaderOpener(custom ? &OpenSelectedDriver
                                                         : nullptr);
}

}  // namespace xdroid
