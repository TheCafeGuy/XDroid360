/**
 * XDroid360 HUD-1 forensic black box (temp, revert-planned). See
 * vulkan_draw_blackbox.h.
 */

#include "xenia/gpu/vulkan/vulkan_draw_blackbox.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/xxhash.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {
namespace vulkan {

namespace {
// Guest physical memory is 512 MB.
constexpr uint32_t kPhysicalSize = 512 * 1024 * 1024;
}  // namespace

uint64_t VulkanDrawBlackbox::HashGuest(const Memory& memory, uint32_t address,
                                       uint32_t length, uint8_t* head) {
  address &= kPhysicalSize - 1;
  length = std::min({length, kHashBytes, kPhysicalSize - address});
  if (!length) {
    return 0;
  }
  const uint8_t* bytes = memory.TranslatePhysical<const uint8_t*>(address);
  if (head) {
    std::memcpy(head, bytes, std::min(length, kHeadBytes));
  }
  return XXH3_64bits(bytes, length);
}

VulkanDrawBlackbox::Record* VulkanDrawBlackbox::Begin(uint64_t frame) {
  Frame& slot = frames_[frame % kRingFrames];
  if (slot.frame != frame) {
    slot.frame = frame;
    slot.count = 0;
    slot.dropped = 0;
    if (slot.records.size() < kMaxDrawsPerFrame) {
      slot.records.resize(kMaxDrawsPerFrame);
    }
  }
  if (slot.count >= kMaxDrawsPerFrame) {
    ++slot.dropped;
    return nullptr;
  }
  Record& record = slot.records[slot.count];
  record = Record();
  record.frame = frame;
  record.draw = slot.count + slot.dropped;
  ++slot.count;
  return &record;
}

void VulkanDrawBlackbox::OnFrameEnd(const Memory& memory, uint64_t frame) {
  Frame& slot = frames_[frame % kRingFrames];
  if (slot.frame != frame) {
    return;
  }
  for (uint32_t i = 0; i < slot.count; ++i) {
    Record& record = slot.records[i];
    if (record.indexed) {
      record.index.hash_swap =
          HashGuest(memory, record.index.address, record.index.length, nullptr);
    }
    for (uint32_t j = 0; j < record.vertex_range_count; ++j) {
      Range& range = record.vertex[j];
      range.hash_swap =
          HashGuest(memory, range.address, range.length, nullptr);
    }
  }
}

void VulkanDrawBlackbox::Dump(const std::filesystem::path& dir, uint64_t n) {
  if (dumps_ >= 100 || n < 3) {
    return;
  }
  std::string text;
  text.reserve(1 << 20);
  auto range_text = [](const char* name, const Range& range, bool head) {
    std::string out = fmt::format(
        " {}={:08X}+{:X} parse={:016X} swap={:016X}{}{}", name, range.address,
        range.length, range.hash_parse, range.hash_swap,
        range.hash_parse != range.hash_swap ? " CHANGED" : "",
        range.fast_path ? " fast" : " upload");
    if (head) {
      out += " head=";
      for (uint32_t i = 0; i < kHeadBytes; ++i) {
        out += fmt::format("{:02X}", range.head[i]);
      }
    }
    return out;
  };
  uint32_t written = 0;
  for (uint64_t f = n - 3; f <= n + 1; ++f) {
    const Frame& slot = frames_[f % kRingFrames];
    if (slot.frame != f) {
      text += fmt::format("# frame {} not in the ring\n", f);
      continue;
    }
    text += fmt::format("# frame {}{}: {} draws recorded, {} dropped\n", f,
                        f == n ? " (HUD-1)" : "", slot.count, slot.dropped);
    for (uint32_t i = 0; i < slot.count; ++i) {
      const Record& record = slot.records[i];
      text += fmt::format("f{} d{} vs={:016X} ps={:016X} prim={} count={}",
                          record.frame, record.draw, record.vs_hash,
                          record.ps_hash, record.primitive,
                          record.index_count);
      if (record.indexed) {
        text += fmt::format(" ifmt={} ioff={}", record.index_format,
                            record.index_offset);
        text += range_text("ib", record.index, false);
      }
      for (uint32_t j = 0; j < record.vertex_range_count; ++j) {
        text += range_text("vb", record.vertex[j], true);
      }
      for (uint32_t j = 0; j < record.texture_count; ++j) {
        const auto& fetch = record.texture_fetch[j];
        text += fmt::format(" t{}={:08X}.{:08X}.{:08X}.{:08X}.{:08X}.{:08X}",
                            record.texture_slot[j], fetch[0], fetch[1],
                            fetch[2], fetch[3], fetch[4], fetch[5]);
      }
      text += '\n';
    }
    ++written;
  }
  if (!written) {
    return;
  }
  char name[40];
  std::snprintf(name, sizeof(name), "draws-f%07llu.txt",
                static_cast<unsigned long long>(n));
  std::filesystem::path path = dir / name;
  ++dumps_;
  XELOGI("XDROID-BLACKBOX: frame {} draw state saved as {}", n,
         xe::path_to_utf8(path));
  std::thread([path, text = std::move(text)] {
    FILE* file = xe::filesystem::OpenFile(path, "wb");
    if (file) {
      std::fwrite(text.data(), 1, text.size(), file);
      std::fclose(file);
    }
  }).detach();
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
