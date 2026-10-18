/**
 * XDroid360 GPU-SAN: cheap sanity checks of the guest state of every draw
 * before it is recorded, reported as EVT-0 events (XdroidEvent).
 *
 * Aimed at PF-0 (a GPU page fault at address 0 in a depth pass) and at
 * corruption whose source is bad guest state: vertex, index, texture and
 * memexport addresses at 0 or outside the 512 MB of guest physical memory,
 * draws the emulator drops without a word, and null descriptors handed to
 * the GPU. Each event carries the draw's shaders, render pass key and its
 * position in the submission, so it can be matched with a breadcrumb report.
 * Switch: xdroid_gpu_sanity (live).
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_GPU_SANITY_H_
#define XENIA_GPU_VULKAN_VULKAN_GPU_SANITY_H_

#include <cstdint>
#include <vector>

#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanGpuSanity {
 public:
  struct DrawId {
    uint64_t vs_hash;
    uint64_t ps_hash;
    uint64_t submission;
    // Draws recorded in the submission before this one.
    uint32_t draw_index;
  };

  // A guest DMA index buffer (CommandProcessor::IndexBufferInfo).
  struct IndexBuffer {
    uint32_t guest_base;
    uint64_t length;
    uint32_t count;
  };

  // Whether the checks run (the switch and an installed event bus).
  static bool enabled();

  // The guest state of a draw about to be recorded.
  static void CheckDraw(
      const RegisterFile& regs, const Shader& vertex_shader,
      const Shader* pixel_shader,
      const IndexBuffer* index_buffer,
      const std::vector<draw_util::MemExportRange>& memexport_ranges,
      const DrawId& id);

  // A draw dropped by the emulator before reaching the GPU. Severity 2 when
  // the drop is a failure, 1 when it is expected (a pipeline still compiling).
  static void Drop(const char* reason, int severity, const DrawId& id);

  // A null image view or sampler about to be written into a descriptor set.
  static void NullDescriptor(const char* kind, bool pixel, uint32_t binding,
                             uint32_t fetch_constant, const DrawId& id);
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_GPU_SANITY_H_
