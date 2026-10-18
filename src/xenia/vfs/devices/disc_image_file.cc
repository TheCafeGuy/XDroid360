/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_image_file.h"

#include <algorithm>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/vfs/devices/disc_image_entry.h"

#if XE_PLATFORM_LINUX
#include <sys/mman.h>
#endif

DEFINE_uint32(vfs_iso_prefetch_kb, 0,
              "Disc image reads: ask the kernel to start reading the request "
              "and this many KiB past it before copying, so storage I/O "
              "overlaps the drive model and guest work. 0 = off (default). "
              "Host-only: does not change what the guest sees or when.",
              "Storage");

namespace xe {
namespace vfs {

DiscImageFile::DiscImageFile(uint32_t file_access, DiscImageEntry* entry)
    : File(file_access, entry), entry_(entry) {}

DiscImageFile::~DiscImageFile() = default;

void DiscImageFile::Destroy() { delete this; }

X_STATUS DiscImageFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_read) {
  if (byte_offset >= entry_->size()) {
    return X_STATUS_END_OF_FILE;
  }

  if (entry_->data_offset() >= entry_->mmap()->size()) {
    xe::FatalError("This ISO image is corrupted and cannot be played.");
    return X_STATUS_END_OF_FILE;
  }

  size_t real_offset = entry_->data_offset() + byte_offset;
  size_t real_length =
      std::min(buffer.size(), entry_->data_size() - byte_offset);
#if XE_PLATFORM_LINUX
  if (cvars::vfs_iso_prefetch_kb) {
    // Page-fault-driven reads of a mapped image (FUSE/SAF on Android) go
    // a page cluster at a time; one WILLNEED queues the whole span instead.
    const size_t map_size = entry_->mmap()->size();
    const size_t span_end = std::min(
        map_size, real_offset + real_length +
                      size_t(cvars::vfs_iso_prefetch_kb) * 1024);
    const size_t page = xe::memory::page_size();
    uint8_t* const base = entry_->mmap()->data();
    const uintptr_t start =
        reinterpret_cast<uintptr_t>(base + real_offset) & ~uintptr_t(page - 1);
    const uintptr_t end = reinterpret_cast<uintptr_t>(base + span_end);
    if (end > start) {
      // Advisory only; a failure just leaves the faults to do the reading.
      madvise(reinterpret_cast<void*>(start), size_t(end - start),
              MADV_WILLNEED);
    }
  }
#endif  // XE_PLATFORM_LINUX
  std::memcpy(buffer.data(), entry_->mmap()->data() + real_offset, real_length);
  *out_bytes_read = real_length;
  return X_STATUS_SUCCESS;
}

}  // namespace vfs
}  // namespace xe
