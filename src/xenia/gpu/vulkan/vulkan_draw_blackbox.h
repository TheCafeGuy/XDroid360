/**
 * XDroid360 HUD-1 forensic black box (temp, revert-planned).
 *
 * Records, per draw with small vertex/index ranges (the 2D HUD class), what
 * the command processor read when it parsed the draw: shaders, primitive,
 * the vertex fetch and index ranges with a hash of their guest bytes, the
 * first bytes of each vertex range, whether the shared memory upload took the
 * all-valid fast path, the texture fetch constants with how each bound
 * texture last got its data, and the float constants its shaders use (only
 * the rows that changed since the previous record of the frame: replay the
 * frame's lines in order for the full set). When the CP reaches
 * the frame's swap it hashes the same ranges again: a parse hash that differs
 * from the swap hash means the guest wrote the data while or after the CP
 * read it (HUD-1 hypotheses H1/H2); a correct parse hash with the fast path
 * taken on changed pages points at a lost invalidation (H3); a stable vertex
 * hash with an unexpected texture fetch points at texture binding (H4).
 * On a HUD-1 detection the frames n-3..n+1 are written as text to
 * <storage>/glitch/draws-fNNNNNNN.txt (docs/runs/2026-10-13/hud1-analysis.md).
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_DRAW_BLACKBOX_H_
#define XENIA_GPU_VULKAN_VULKAN_DRAW_BLACKBOX_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "xenia/gpu/texture_cache.h"

namespace xe {
class Memory;
namespace gpu {
namespace vulkan {

class VulkanDrawBlackbox {
 public:
  static constexpr uint32_t kMaxRanges = 8;
  static constexpr uint32_t kMaxTextures = 4;
  static constexpr uint32_t kHeadBytes = 32;
  // Ranges above this are not HUD-class; hashes cover at most kHashBytes.
  static constexpr uint32_t kMaxRangeBytes = 64 * 1024;
  static constexpr uint32_t kHashBytes = 64 * 1024;

  struct Range {
    uint32_t address = 0;
    uint32_t length = 0;
    uint64_t hash_parse = 0;
    uint64_t hash_swap = 0;
    bool fast_path = false;
    uint8_t head[kHeadBytes] = {};
  };
  struct Record {
    uint64_t frame = 0;
    uint32_t draw = 0;
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;
    uint32_t primitive = 0;
    uint32_t index_count = 0;
    uint32_t index_offset = 0;
    uint32_t index_format = 0;
    bool indexed = false;
    Range index;
    uint32_t vertex_range_count = 0;
    std::array<Range, kMaxRanges> vertex;
    uint32_t texture_count = 0;
    std::array<uint32_t, kMaxTextures> texture_slot{};
    std::array<std::array<uint32_t, 6>, kMaxTextures> texture_fetch{};
    std::array<TextureCache::XdroidLoadState, kMaxTextures> texture_state{};
    // Indirect buffers the draw was parsed in, outermost first (ptr, dwords).
    uint32_t ib_depth = 0;
    std::array<std::array<uint32_t, 2>, 3> ib_chain{};
    // Changed float constants: constant_count entries of 5 words (row, with
    // 256 added for the pixel shader, and its value) from word constant_begin
    // of the frame's constants.
    uint32_t constant_begin = 0;
    uint32_t constant_count = 0;
  };

  // Whether a draw with this much vertex+index data is recorded.
  static bool Wanted(uint32_t vertex_bytes, uint32_t index_bytes) {
    return vertex_bytes <= kMaxRangeBytes && index_bytes <= kMaxRangeBytes;
  }
  static uint64_t HashGuest(const Memory& memory, uint32_t address,
                            uint32_t length, uint8_t* head);

  // Returns the slot to fill for a draw of |frame|, or nullptr when the frame
  // is full. Draws are numbered in parse order within the frame.
  Record* Begin(uint64_t frame);
  // Adds to |record| the float constant rows set in |bitmap| (256 bits) whose
  // value in |rows| differs from the last one recorded this frame. |base| is
  // 0 for the vertex shader and 256 for the pixel shader; vertex first.
  void AddConstants(Record& record, const uint32_t* rows,
                    const uint64_t* bitmap, uint32_t base);
  // CP thread at the swap that closes |frame|: re-hashes its ranges.
  void OnFrameEnd(const Memory& memory, uint64_t frame);
  // Writes frames n-3..n+1 if they are still in the ring.
  void Dump(const std::filesystem::path& dir, uint64_t n);

 private:
  static constexpr uint32_t kRingFrames = 8;
  static constexpr uint32_t kMaxDrawsPerFrame = 3072;
  struct Frame {
    uint64_t frame = UINT64_MAX;
    uint32_t count = 0;
    uint32_t dropped = 0;
    std::vector<Record> records;
    std::vector<uint32_t> constants;
  };
  std::array<Frame, kRingFrames> frames_;
  // Float constant values last recorded in the frame being parsed.
  std::array<uint32_t, 512 * 4> rows_{};
  std::array<uint64_t, 512 / 64> rows_known_{};
  uint32_t dumps_ = 0;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_DRAW_BLACKBOX_H_
