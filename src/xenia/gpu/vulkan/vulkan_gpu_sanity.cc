#include "xenia/gpu/vulkan/vulkan_gpu_sanity.h"

#include <functional>
#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/base/math.h"
#include "xenia/gpu/xdroid_gpu_hooks.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(xdroid_gpu_sanity, true,
            "XDroid360 GPU-SAN: check the guest addresses of every draw "
            "(vertex, index, texture, memexport at 0 or past guest memory), "
            "count draws the emulator drops and null descriptors, and report "
            "each new case as an XDROID-EVT line (severity 2+ auto-marks).",
            "XDroid360");

namespace xe {
namespace gpu {
namespace vulkan {

namespace {

// Guest physical memory the GPU can address.
constexpr uint64_t kPhysicalBytes = UINT64_C(512) << 20;

uint64_t Mix(uint64_t a, uint64_t b) {
  return (a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2))) *
         0xFF51AFD7ED558CCDull;
}

// Whether [address, address + size) leaves guest physical memory, address
// masked to physical like the shaders do (guests use mirror windows).
bool OutOfRange(uint32_t address, uint64_t size) {
  return uint64_t(xenos::CpuToGpu(address)) + size > kPhysicalBytes;
}

void CheckTextures(const RegisterFile& regs, const Shader& shader, bool pixel,
                   const VulkanGpuSanity::DrawId& id) {
  for (const Shader::TextureBinding& binding : shader.texture_bindings()) {
    const xenos::xe_gpu_texture_fetch_t fetch =
        regs.GetTextureFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kTexture ||
        fetch.base_address) {
      continue;
    }
    XdroidEvent("san-tfetch-null", 1,
                Mix(shader.ucode_data_hash(), binding.fetch_constant),
                "{} texture fetch {} base 0 (format {}, dwords {:08X} {:08X}) "
                "VS {:016X} PS {:016X} submission {} draw {}",
                pixel ? "PS" : "VS", binding.fetch_constant,
                uint32_t(fetch.format), fetch.dword_0, fetch.dword_1,
                id.vs_hash, id.ps_hash, id.submission, id.draw_index);
  }
}

}  // namespace

bool VulkanGpuSanity::enabled() {
  return cvars::xdroid_gpu_sanity &&
         GetXdroidGpuHooks().on_event.load(std::memory_order_relaxed);
}

void VulkanGpuSanity::CheckDraw(
    const RegisterFile& regs, const Shader& vertex_shader,
    const Shader* pixel_shader, const IndexBuffer* index_buffer_info,
    const std::vector<draw_util::MemExportRange>& memexport_ranges,
    const DrawId& id) {
  const Shader::ConstantRegisterMap& constant_map =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < xe::countof(constant_map.vertex_fetch_bitmap); ++i) {
    uint32_t bits = constant_map.vertex_fetch_bitmap[i];
    uint32_t j;
    while (xe::bit_scan_forward(bits, &j)) {
      bits = xe::clear_lowest_bit(bits);
      const uint32_t index = i * 32 + j;
      const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(index);
      if (fetch.type != xenos::FetchConstantType::kVertex) {
        // Invalid types are refused (and logged) by IssueDraw itself.
        continue;
      }
      const uint32_t address = fetch.address << 2;
      const uint64_t size = uint64_t(fetch.size) << 2;
      const char* type = nullptr;
      int severity = 2;
      if (!address) {
        type = "san-vfetch-null";
      } else if (OutOfRange(address, size)) {
        type = "san-vfetch-oob";
      } else if (!size) {
        type = "san-vfetch-empty";
        severity = 1;
      }
      if (type) {
        XdroidEvent(type, severity, Mix(id.vs_hash, index),
                    "vertex fetch {} address {:08X} size {} (dwords {:08X} "
                    "{:08X}) VS {:016X} PS {:016X} submission {} draw {}",
                    index, address, size, fetch.dword_0, fetch.dword_1,
                    id.vs_hash, id.ps_hash, id.submission, id.draw_index);
      }
    }
  }

  if (index_buffer_info) {
    const char* type = nullptr;
    if (!index_buffer_info->guest_base) {
      type = "san-index-null";
    } else if (OutOfRange(index_buffer_info->guest_base,
                          index_buffer_info->length)) {
      type = "san-index-oob";
    }
    if (type) {
      XdroidEvent(type, 2, Mix(id.vs_hash, index_buffer_info->guest_base),
                  "index buffer {:08X} length {} count {} VS {:016X} PS "
                  "{:016X} submission {} draw {}",
                  index_buffer_info->guest_base, index_buffer_info->length,
                  index_buffer_info->count, id.vs_hash, id.ps_hash,
                  id.submission, id.draw_index);
    }
  }

  for (const draw_util::MemExportRange& range : memexport_ranges) {
    const uint32_t address = range.base_address_dwords << 2;
    if (address && !OutOfRange(address, range.size_bytes)) {
      continue;
    }
    XdroidEvent(address ? "san-memexport-oob" : "san-memexport-null", 2,
                Mix(Mix(id.vs_hash, id.ps_hash), address),
                "memexport {:08X} size {} VS {:016X} PS {:016X} submission {} "
                "draw {}",
                address, range.size_bytes, id.vs_hash, id.ps_hash,
                id.submission, id.draw_index);
  }

  CheckTextures(regs, vertex_shader, false, id);
  if (pixel_shader) {
    CheckTextures(regs, *pixel_shader, true, id);
  }
}

void VulkanGpuSanity::Drop(const char* reason, int severity, const DrawId& id) {
  XdroidEvent(
      "san-drop", severity,
      Mix(Mix(id.vs_hash, id.ps_hash), std::hash<std::string_view>()(reason)),
      "draw dropped: {} VS {:016X} PS {:016X} submission {} draw {}", reason,
      id.vs_hash, id.ps_hash, id.submission, id.draw_index);
}

void VulkanGpuSanity::NullDescriptor(const char* kind, bool pixel,
                                     uint32_t binding, uint32_t fetch_constant,
                                     const DrawId& id) {
  XdroidEvent("san-desc-null", 3,
              Mix(Mix(pixel ? id.ps_hash : id.vs_hash, binding),
                  std::hash<std::string_view>()(kind)),
              "null {} for {} binding {} (fetch {}) VS {:016X} PS {:016X} "
              "submission {} draw {}",
              kind, pixel ? "PS" : "VS", binding, fetch_constant, id.vs_hash,
              id.ps_hash, id.submission, id.draw_index);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
