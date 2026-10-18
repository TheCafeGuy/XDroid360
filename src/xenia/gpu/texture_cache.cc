/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/texture_cache.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/shared_memory.h"

DEFINE_bool(log_samplers, false,
            "Log the sampler parameters each backend derives from a fetch "
            "constant.",
            "GPU.Debug");

DEFINE_bool(log_texture_loads, false,
            "Log every texture upload with its guest key and the load "
            "shader the backend picked for it.",
            "GPU.Debug");

DEFINE_int32(
    draw_resolution_scale_x, 1,
    "Integer pixel width scale used for scaling the rendering resolution "
    "opaquely to the game.\n"
    "Values from 1 to 7 may be supported, depending on device capabilities. "
    "Requires sparse binding (Vulkan) or tiled resources (D3D12) for scales "
    "above 1x1. The emulator will automatically clamp to the maximum supported "
    "scale if the requested value exceeds device limits.\n"
    "Various effects and parts of game rendering pipelines may work "
    "incorrectly as pixels become ambiguous from the game's perspective and "
    "because half-pixel offset (which normally doesn't affect coverage when "
    "MSAA isn't used) becomes full-pixel.",
    "GPU");
DEFINE_int32(
    draw_resolution_scale_y, 1,
    "Integer pixel width scale used for scaling the rendering resolution "
    "opaquely to the game.\n"
    "See draw_resolution_scale_x for more information.",
    "GPU");
DEFINE_uint32(
    texture_cache_memory_limit_soft, 384,
    "Maximum host texture memory usage (in megabytes) above which old textures "
    "will be destroyed.",
    "GPU.Debug");
DEFINE_uint32(
    texture_cache_memory_limit_soft_lifetime, 30,
    "Seconds a texture should be unused to be considered old enough to be "
    "deleted if texture memory usage exceeds texture_cache_memory_limit_soft.",
    "GPU.Debug");
DEFINE_uint32(
    texture_cache_memory_limit_hard, 768,
    "Maximum host texture memory usage (in megabytes) above which textures "
    "will be destroyed as soon as possible.",
    "GPU.Debug");
DEFINE_uint32(
    texture_cache_memory_limit_render_to_texture, 24,
    "Part of the host texture memory budget (in megabytes) that will be scaled "
    "by the current drawing resolution scale.\n"
    "If texture_cache_memory_limit_soft, for instance, is 384, and this is 24, "
    "it will be assumed that the game will be using roughly 24 MB of "
    "render-to-texture (resolve) targets and 384 - 24 = 360 MB of regular "
    "textures - so with 2x2 resolution scaling, the soft limit will be 360 + "
    "96 MB, and with 3x3, it will be 360 + 216 MB.",
    "GPU.Debug");
DEFINE_bool(tiled_shared_memory, true,
            "Enable tiled/sparse resources for efficient large address space "
            "support. Disable for graphics debugger compatibility.",
            "GPU.Debug");
DEFINE_bool(xdroid_tex_probe, false,
            "TEX-1 forensics: log every texture first-use miss as an "
            "XDROID-TEXMISS line (create/load failure, texture bound before "
            "any load, first load from zeroed memory, data rewritten right "
            "after the first load, budget eviction thrash, async pipeline "
            "stand-ins for textured draws), plus a summary every 300 frames. "
            "Live.",
            "GPU");
DEFINE_bool(xdroid_tex_census, false,
            "TEX-1 census: per texture key, count the loads (first, base, "
            "mips), what invalidated them (CPU or GPU write), whether the "
            "reloaded guest bytes were identical, bytes and CPU time; every "
            "300 frames log XDROID-TEXCENSUS totals and the top 12 keys. Live.",
            "GPU");
DEFINE_bool(xdroid_tex_skip_same, false,
            "TEX-1a: when a CPU write invalidated a texture but its guest "
            "bytes hash the same as at its last load, keep the host texture "
            "instead of loading it again (the shared memory upload still "
            "happens). Live.",
            "GPU");
DEFINE_bool(xdroid_tex_unloaded_null, false,
            "TEX-1 candidate fix: bind the null (zero) image instead of a "
            "texture whose data has never loaded, whose contents are "
            "undefined (on a recycled allocation, another texture). Live.",
            "GPU");

namespace xe {
namespace gpu {

const TextureCache::LoadShaderInfo
    TextureCache::load_shader_info_[kLoadShaderCount] = {
        {1, 4},   // k8bpb
        {2, 4},   // k16bpb
        {4, 3},   // k32bpb
        {8, 2},   // k64bpb
        {16, 1},  // k128bpb
        {2, 4},   // kR5G5B5A1ToB5G5R5A1
        {2, 4},   // kR5G6B5ToB5G6R5
        {2, 4},   // kR5G5B6ToB5G6R5WithRBGASwizzle
        {2, 4},   // kRGBA4ToBGRA4
        {2, 4},   // kRGBA4ToARGB4
        {4, 3},   // kGBGR8ToGRGB8
        {8, 3},   // kGBGR8ToRGB8
        {4, 3},   // kBGRG8ToRGBG8
        {8, 3},   // kBGRG8ToRGB8
        {8, 3},   // kR10G11B11ToRGBA16
        {8, 3},   // kR10G11B11ToRGBA16SNorm
        {8, 3},   // kR11G11B10ToRGBA16
        {8, 3},   // kR11G11B10ToRGBA16SNorm
        {2, 4},   // kR16UNormToFloat
        {2, 4},   // kR16SNormToFloat
        {4, 3},   // kRG16UNormToFloat
        {4, 3},   // kRG16SNormToFloat
        {8, 2},   // kRGBA16UNormToFloat
        {8, 2},   // kRGBA16SNormToFloat
        {4, 2},   // kDXT1ToRGBA8
        {4, 1},   // kDXT3ToRGBA8
        {4, 1},   // kDXT5ToRGBA8
        {2, 1},   // kDXNToRG8
        {1, 2},   // kDXT3A
        {2, 2},   // kDXT3AAs1111ToBGRA4
        {2, 2},   // kDXT3AAs1111ToARGB4
        {1, 2},   // kDXT5AToR8
        {2, 2},   // kCTX1
        {4, 3},   // kDepthUnorm
        {4, 3},   // kDepthFloat
};

TextureCache::TextureCache(const RegisterFile& register_file,
                           SharedMemory& shared_memory,
                           TraceWriter* trace_writer,
                           uint32_t draw_resolution_scale_x,
                           uint32_t draw_resolution_scale_y)
    : register_file_(register_file),
      shared_memory_(shared_memory),
      trace_writer_(trace_writer),
      draw_resolution_scale_x_(draw_resolution_scale_x),
      draw_resolution_scale_y_(draw_resolution_scale_y),
      draw_resolution_scale_x_divisor_(draw_resolution_scale_x),
      draw_resolution_scale_y_divisor_(draw_resolution_scale_y) {
  assert_true(draw_resolution_scale_x >= 1);
  assert_true(draw_resolution_scale_x <= kMaxDrawResolutionScaleAlongAxis);
  assert_true(draw_resolution_scale_y >= 1);
  assert_true(draw_resolution_scale_y <= kMaxDrawResolutionScaleAlongAxis);

  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    constexpr uint32_t kScaledResolvePageDwordCount =
        SharedMemory::kBufferSize / 4096 / 32;
    scaled_resolve_pages_ =
        std::unique_ptr<uint32_t[]>(new uint32_t[kScaledResolvePageDwordCount]);
    std::memset(scaled_resolve_pages_.get(), 0,
                kScaledResolvePageDwordCount * sizeof(uint32_t));
    std::memset(scaled_resolve_pages_l2_, 0, sizeof(scaled_resolve_pages_l2_));
    scaled_resolve_global_watch_handle_ = shared_memory.RegisterGlobalWatch(
        ScaledResolveGlobalWatchCallbackThunk, this);
  }
}

TextureCache::~TextureCache() {
  DestroyAllTextures(true);

  if (scaled_resolve_global_watch_handle_) {
    shared_memory().UnregisterGlobalWatch(scaled_resolve_global_watch_handle_);
  }
}

bool TextureCache::GetConfigDrawResolutionScale(uint32_t& x_out,
                                                uint32_t& y_out) {
  uint32_t config_x =
      uint32_t(std::max(INT32_C(1), cvars::draw_resolution_scale_x));
  uint32_t config_y =
      uint32_t(std::max(INT32_C(1), cvars::draw_resolution_scale_y));

  uint32_t clamped_x = std::min(kMaxDrawResolutionScaleAlongAxis, config_x);
  uint32_t clamped_y = std::min(kMaxDrawResolutionScaleAlongAxis, config_y);
  x_out = clamped_x;
  y_out = clamped_y;
  return clamped_x == config_x && clamped_y == config_y;
}

bool TextureCache::ClampDrawResolutionScaleToMaxSupported(
    uint32_t& scale_x, uint32_t& scale_y, bool sparse_bind_supported,
    uint32_t virtual_address_bits_per_resource) {
  // Without sparse/tiled resource support, resolution scaling is not possible
  // because the scaled address space exceeds what simple buffers can handle.
  if (!sparse_bind_supported) {
    bool was_clamped = scale_x > 1 || scale_y > 1;
    scale_x = 1;
    scale_y = 1;
    return !was_clamped;
  }

  // With sparse binding, limit based on virtual address space if specified.
  bool was_clamped = false;
  if (virtual_address_bits_per_resource > 0) {
    while (scale_x > 1 || scale_y > 1) {
      uint64_t highest_scaled_address =
          uint64_t(SharedMemory::kBufferSize) * (scale_x * scale_y) - 1;
      if (uint32_t(64) - xe::lzcnt(highest_scaled_address) <=
          virtual_address_bits_per_resource) {
        break;
      }
      // When reducing from a square size, prefer decreasing the horizontal
      // resolution as vertical resolution difference is visible more clearly in
      // perspective.
      was_clamped = true;
      if (scale_x >= scale_y) {
        --scale_x;
      } else {
        --scale_y;
      }
    }
  }

  return !was_clamped;
}

void TextureCache::LogSamplerParameters(uint32_t fetch_constant,
                                        uint32_t packed) const {
  if (!cvars::log_samplers) {
    return;
  }
  XELOGI(
      "log_samplers: fetch {}, value 0x{:08X}, clamp {}/{}/{}, border {}, "
      "linear mag {} min {} mip {}, aniso {}, mip min level {}, base map {}",
      fetch_constant, packed, packed & 0x7, (packed >> 3) & 0x7,
      (packed >> 6) & 0x7, (packed >> 9) & 0x3, (packed >> 11) & 0x1,
      (packed >> 12) & 0x1, (packed >> 13) & 0x1, (packed >> 14) & 0x7,
      (packed >> 17) & 0xF, (packed >> 21) & 0x1);
}

void TextureCache::LogTextureLoad(const TextureKey& key, uint32_t load_shader,
                                  bool load_base, bool load_mips) const {
  if (!cvars::log_texture_loads) {
    return;
  }
  XELOGI(
      "log_texture_loads: base 0x{:08X} mips 0x{:08X}, {}x{}x{}, {} mips, "
      "{}, pitch {}, {}{}{}, load shader {}, loading {}{}",
      key.base_page << 12, key.mip_page << 12, key.width_minus_1 + 1,
      key.height_minus_1 + 1, key.depth_or_array_size_minus_1 + 1,
      key.mip_max_level + 1, FormatInfo::GetName(key.format), key.pitch,
      key.tiled ? "tiled" : "linear", key.packed_mips ? ", packed mips" : "",
      key.signed_separate ? ", signed" : "", load_shader,
      load_base ? "base" : "", load_mips ? " mips" : "");
}

void TextureCache::ClearCache() { DestroyAllTextures(); }

void TextureCache::CompletedSubmissionUpdated(
    uint64_t completed_submission_index) {
  // If memory usage is too high, destroy unused textures.
  uint64_t current_time = xe::Clock::QueryHostUptimeMillis();
  // texture_cache_memory_limit_render_to_texture is assumed to be included in
  // texture_cache_memory_limit_soft and texture_cache_memory_limit_hard, at 1x,
  // so subtracting 1 from the scale.
  uint32_t limit_scaled_resolve_add_mb =
      cvars::texture_cache_memory_limit_render_to_texture *
      (draw_resolution_scale_x() * draw_resolution_scale_y() - 1);
  uint32_t limit_soft_mb =
      cvars::texture_cache_memory_limit_soft + limit_scaled_resolve_add_mb;
  uint32_t limit_hard_mb =
      cvars::texture_cache_memory_limit_hard + limit_scaled_resolve_add_mb;
  uint32_t limit_soft_lifetime =
      cvars::texture_cache_memory_limit_soft_lifetime * 1000;
  bool destroyed_any = false;
  while (texture_used_first_ != nullptr) {
    uint64_t total_host_memory_usage_mb =
        (textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20;
    bool limit_hard_exceeded = total_host_memory_usage_mb > limit_hard_mb;
    if (total_host_memory_usage_mb <= limit_soft_mb && !limit_hard_exceeded) {
      break;
    }
    Texture* texture = texture_used_first_;
    if (texture->last_usage_submission_index() > completed_submission_index) {
      break;
    }
    if (!limit_hard_exceeded &&
        (texture->last_usage_time() + limit_soft_lifetime) > current_time) {
      break;
    }
    if (!destroyed_any) {
      destroyed_any = true;
      // The texture being destroyed might have been bound in the previous
      // submissions, and nothing has overwritten the binding yet, so completion
      // of the submission where the texture was last actually used on the GPU
      // doesn't imply that it's not bound currently. Reset bindings if
      // any texture has been destroyed.
      ResetTextureBindings();
    }
    if (cvars::xdroid_tex_probe) {
      ++probe_counters_.evicted;
      if (probe_evicted_.size() >= 4096) {
        probe_evicted_.clear();
      }
      probe_evicted_[texture->key()] = probe_frame();
    }
    // Remove the texture from the map and destroy it via its unique_ptr.
    auto found_texture_it = textures_.find(texture->key());
    assert_true(found_texture_it != textures_.end());
    if (found_texture_it != textures_.end()) {
      assert_true(found_texture_it->second.get() == texture);
      textures_.erase(found_texture_it);
      // `texture` is invalid now.
    }
  }
  if (destroyed_any) {
    COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  }
}

void TextureCache::BeginSubmission(uint64_t new_submission_index) {
  assert_true(new_submission_index > current_submission_index_);
  current_submission_index_ = new_submission_index;
  current_submission_time_ = xe::Clock::QueryHostUptimeMillis();
}

void TextureCache::BeginFrame() {
  // In case there was a failure to create something in the previous frame, make
  // sure bindings are reset so a new attempt will surely be made if the texture
  // is requested again.
  ResetTextureBindings();
  unloaded_null_slots_ = 0;
  probe_frame_.fetch_add(1, std::memory_order_relaxed);
  if (cvars::xdroid_tex_probe) {
    ProbeMaybeSummary();
  }
  if (cvars::xdroid_tex_census) {
    CensusMaybeSummary();
  } else if (!census_.empty()) {
    census_.clear();
  }
}

bool TextureCache::IsTextureHiddenAsUnloaded(const Texture* texture) {
  return texture && !texture->ever_loaded() && cvars::xdroid_tex_unloaded_null;
}

void TextureCache::ProbeNoteStandIn(ProbeStandIn kind) {
  ++probe_counters_.stand_in[uint32_t(kind)];
}

void TextureCache::ProbeLog(const char* event, const TextureKey& key,
                            const char* detail) {
  XELOGW(
      "XDROID-TEXMISS {} frame={} base=0x{:08X} mips=0x{:08X} {}x{}x{} {} {} "
      "fmt={} mips={} {}",
      event, probe_frame(), key.base_page << 12, key.mip_page << 12,
      key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(),
      key.GetLogDimensionName(), key.tiled ? "tiled" : "linear",
      FormatInfo::GetName(key.format), key.mip_max_level + 1, detail);
}

void TextureCache::ProbeFirstLoad(Texture& texture) {
  texture.ever_loaded_ = true;
  if (!cvars::xdroid_tex_probe) {
    return;
  }
  texture.probe_.first_load_frame = probe_frame();
  // Data that hasn't arrived yet: sample the guest memory the load read, 32
  // probes of 16 bytes over the base level (or the mips without a base).
  const TextureKey& key = texture.key();
  uint32_t size = texture.GetGuestBaseSize();
  uint32_t start = key.base_page << 12;
  if (!size) {
    size = texture.GetGuestMipsSize();
    start = key.mip_page << 12;
  }
  // Render-to-texture data lives on the GPU; guest RAM may lack it.
  if (size < 16 || key.scaled_resolve ||
      shared_memory().IsRangeAnyGpuWritten(start, size)) {
    return;
  }
  if (shared_memory().IsRangeSampledZero(start, size)) {
    ++probe_counters_.zero_first;
    ProbeLog("zero_first", key,
             "reason=first load read zeroed guest memory (data not there yet)");
  }
}

void TextureCache::ProbeLoadResult(Texture& texture,
                                   const char* fail_reason) {
  if (!fail_reason) {
    if (!texture.ever_loaded()) {
      ProbeFirstLoad(texture);
    } else if (texture.probe_.early_invalidation && cvars::xdroid_tex_probe) {
      ++probe_counters_.early_reload;
      ProbeLog("early_reload", texture.key(),
               fmt::format("reason=data rewritten by the {} within {} frames "
                           "of the first load (first use drew stale data)",
                           (texture.probe_.early_invalidation & 2) ? "GPU"
                                                                   : "CPU",
                           probe_frame() - texture.probe_.first_load_frame)
                   .c_str());
      texture.probe_.early_invalidation = 0;
    }
    if (cvars::xdroid_tex_probe) {
      ++probe_counters_.loads;
    }
    return;
  }
  if (!cvars::xdroid_tex_probe) {
    return;
  }
  ++probe_counters_.load_fail;
  ProbeLog("load_fail", texture.key(),
           fmt::format("reason={} ever_loaded={}", fail_reason,
                       texture.ever_loaded() ? 1 : 0)
               .c_str());
}

void TextureCache::ProbeMaybeSummary() {
  uint64_t frame = probe_frame();
  if (frame - probe_summary_frame_ < 300) {
    return;
  }
  probe_summary_frame_ = frame;
  const ProbeCounters& c = probe_counters_;
  XELOGW(
      "XDROID-TEXMISS summary frame={} textures={} mem_mb={} created={} "
      "loads={} create_fail={} load_fail={} unwatched={} bound_unloaded={} "
      "zero_first={} early_reload={} evicted={} evict_recreate={} "
      "standin_placeholder={} standin_skipped={} standin_waited={} "
      "standin_wait_timeout={}",
      frame, textures_.size(),
      (textures_total_host_memory_usage_ + ((UINT32_C(1) << 20) - 1)) >> 20,
      c.created, c.loads, c.create_fail, c.load_fail, c.unwatched,
      c.bound_unloaded, c.zero_first, c.early_reload, c.evicted,
      c.evict_recreate, c.stand_in[0], c.stand_in[1], c.stand_in[2],
      c.stand_in[3]);
  probe_counters_ = ProbeCounters();
}

bool TextureCache::CompleteResolveLink(Texture& texture) {
  // The resolve's watch callback has just marked the base outdated (and
  // dropped the watch); the copy recorded before it made the image current.
  auto global_lock = global_critical_region_.Acquire();
  if (!texture.MakeUpToDateAndWatch(global_lock)) {
    return false;
  }
  // The bytes are GPU-written: no hash for TEX-1a to match against.
  texture.census_.base_hash = 0;
  texture.census_.inval = 0;
  return true;
}

void TextureCache::CensusNoteResolve(const CensusResolve& resolve) {
  if (!cvars::xdroid_tex_census || !resolve.extent_length) {
    return;
  }
  CensusResolve& r = census_resolves_[resolve.extent_start >> 12];
  uint32_t resolves = r.resolves;
  r = resolve;
  r.resolves = resolves + 1;
}

TextureCache::CensusLink TextureCache::CensusClassifyLink(
    const Texture& texture, bool& multi) {
  multi = false;
  const TextureKey& key = texture.key();
  auto it = census_resolves_.find(key.base_page);
  if (it == census_resolves_.end()) {
    return kCensusLinkNoRecord;
  }
  CensusResolve& r = it->second;
  multi = r.resolves > 1;
  r.resolves = 0;
  if (key.dimension != xenos::DataDimension::k2DOrStacked ||
      key.depth_or_array_size_minus_1 || !key.tiled || key.scaled_resolve) {
    return kCensusLinkShape;
  }
  if (key.mip_max_level || key.packed_mips) {
    return kCensusLinkMips;
  }
  if (!r.fast) {
    return kCensusLinkNotFast;
  }
  if (r.offset_x_div_8 || r.offset_y_div_8 ||
      r.extent_start != (uint32_t(key.base_page) << 12)) {
    return kCensusLinkOffset;
  }
  if (r.pitch_div_32 != key.pitch) {
    return kCensusLinkPitch;
  }
  if (r.height_div_32 != (key.GetHeight() + 31) / 32) {
    return kCensusLinkHeight;
  }
  if (r.format != uint32_t(key.format)) {
    return kCensusLinkFormat;
  }
  if (r.endian != uint32_t(key.endianness)) {
    return kCensusLinkEndian;
  }
  if (r.extent_length < texture.GetGuestBaseSize()) {
    return kCensusLinkCoverage;
  }
  return r.direct ? kCensusLinkDirect : kCensusLinkEdram;
}

void TextureCache::CensusRecord(Texture& texture, uint8_t inval, bool base,
                                bool mips, bool same, bool skipped,
                                uint64_t bytes, uint64_t ns) {
  CensusEntry& e = census_[texture.key()];
  e.key = texture.key();
  ++e.loads;
  if (!texture.ever_loaded()) {
    ++e.first;
  } else if (inval & 0b1010) {
    ++e.inval_gpu;
    bool multi;
    CensusLink link = CensusClassifyLink(texture, multi);
    e.link = link;
    ++census_link_[link];
    census_link_bytes_[link] += bytes;
    census_link_multi_ += multi ? 1 : 0;
  } else if (inval & 0b0101) {
    ++e.inval_cpu;
  }
  e.base += base ? 1 : 0;
  e.mips += mips ? 1 : 0;
  e.same += same ? 1 : 0;
  e.skipped += skipped ? 1 : 0;
  e.bytes += bytes;
  e.ns += ns;
}

void TextureCache::CensusMaybeSummary() {
  uint64_t frame = probe_frame();
  if (frame - census_summary_frame_ < 300) {
    return;
  }
  uint64_t frames = frame - census_summary_frame_;
  census_summary_frame_ = frame;
  if (census_.empty() || frames > 600) {
    // First window after enabling: the start frame is unknown.
    census_.clear();
    return;
  }
  std::vector<const CensusEntry*> entries;
  entries.reserve(census_.size());
  CensusEntry total;
  for (const auto& it : census_) {
    const CensusEntry& e = it.second;
    entries.push_back(&e);
    total.loads += e.loads;
    total.first += e.first;
    total.inval_cpu += e.inval_cpu;
    total.inval_gpu += e.inval_gpu;
    total.same += e.same;
    total.skipped += e.skipped;
    total.bytes += e.bytes;
    total.ns += e.ns;
  }
  XELOGW(
      "XDROID-TEXCENSUS summary frame={} frames={} keys={} loads={} "
      "loads_per_frame={:.1f} first={} inval_cpu={} inval_gpu={} same={} "
      "skipped={} kb_per_frame={} cpu_us_per_frame={}",
      frame, frames, census_.size(), total.loads,
      double(total.loads) / double(frames), total.first, total.inval_cpu,
      total.inval_gpu, total.same, total.skipped,
      (total.bytes >> 10) / frames,
      total.ns / 1000 / frames);
  static const char* const kLinkNames[kCensusLinkCount] = {
      "direct", "edram", "no_record", "not_fast", "offset", "pitch",
      "height", "format", "endian", "coverage", "mips", "shape"};
  std::string link_line;
  for (uint32_t i = 0; i < kCensusLinkCount; ++i) {
    link_line += fmt::format(" {}={}/{}kb", kLinkNames[i], census_link_[i],
                             (census_link_bytes_[i] >> 10) / frames);
  }
  XELOGW("XDROID-TEXCENSUS link frames={} multi={}{}", frames,
         census_link_multi_, link_line);
  std::fill(std::begin(census_link_), std::end(census_link_), 0);
  std::fill(std::begin(census_link_bytes_), std::end(census_link_bytes_), 0);
  census_link_multi_ = 0;
  census_resolves_.clear();
  size_t top = std::min(entries.size(), size_t(12));
  std::partial_sort(entries.begin(), entries.begin() + top, entries.end(),
                    [](const CensusEntry* a, const CensusEntry* b) {
                      return a->loads > b->loads;
                    });
  for (size_t i = 0; i < top; ++i) {
    const CensusEntry& e = *entries[i];
    const TextureKey& key = e.key;
    XELOGW(
        "XDROID-TEXCENSUS top={} loads={} first={} base={} mips={} cpu={} "
        "gpu={} same={} skipped={} link={} kb={} us={} base=0x{:08X} "
        "mips=0x{:08X} {}x{}x{} {} {} fmt={} levels={}{}",
        i, e.loads, e.first, e.base, e.mips, e.inval_cpu, e.inval_gpu, e.same,
        e.skipped, e.link < kCensusLinkCount ? kLinkNames[e.link] : "-",
        e.bytes >> 10, e.ns / 1000, key.base_page << 12, key.mip_page << 12,
        key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(),
        key.GetLogDimensionName(), key.tiled ? "tiled" : "linear",
        FormatInfo::GetName(key.format), key.mip_max_level + 1,
        key.scaled_resolve ? " scaled" : "");
  }
  census_.clear();
}

void TextureCache::MarkRangeAsResolved(uint32_t start_unscaled,
                                       uint32_t length_unscaled,
                                       bool resolution_scaled) {
  if (length_unscaled == 0) {
    return;
  }
  start_unscaled &= 0x1FFFFFFF;
  length_unscaled = std::min(length_unscaled, 0x20000000 - start_unscaled);

  if (IsDrawResolutionScaled()) {
    uint32_t page_first = start_unscaled >> 12;
    uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
    uint32_t block_first = page_first >> 5;
    uint32_t block_last = page_last >> 5;
    auto global_lock = global_critical_region_.Acquire();
    for (uint32_t i = block_first; i <= block_last; ++i) {
      uint32_t add_bits = UINT32_MAX;
      if (i == block_first) {
        add_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (i == block_last && (page_last & 31) != 31) {
        add_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      if (resolution_scaled) {
        scaled_resolve_pages_[i] |= add_bits;
        scaled_resolve_pages_l2_[i >> 6] |= UINT64_C(1) << (i & 63);
      } else {
        // Native resolve data is in shared memory.
        // Clear the same way the CPU write watch does.
        scaled_resolve_pages_[i] &= ~add_bits;
        if (!scaled_resolve_pages_[i]) {
          scaled_resolve_pages_l2_[i >> 6] &= ~(UINT64_C(1) << (i & 63));
        }
      }
    }
    // What the shared memory buffer holds here isn't any earlier resolve's
    // output anymore. A scaled resolve mirrored there is recorded again after.
    for (auto it = scaled_resolve_extents_.begin();
         it != scaled_resolve_extents_.end() && it->first <= page_last;) {
      if (it->second >= page_first) {
        it = scaled_resolve_extents_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Invalidate textures. Toggling individual textures between scaled and
  // unscaled also relies on invalidation through shared memory.
  shared_memory().RangeWrittenByGpu(start_unscaled, length_unscaled);
}

void TextureCache::MarkScaledResolveMirrored(uint32_t start_unscaled,
                                             uint32_t length_unscaled) {
  if (!IsDrawResolutionScaled() || !length_unscaled) {
    return;
  }
  start_unscaled &= 0x1FFFFFFF;
  length_unscaled = std::min(length_unscaled, 0x20000000 - start_unscaled);
  uint32_t page_first = start_unscaled >> 12;
  uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
  auto global_lock = global_critical_region_.Acquire();
  // Past the limit, a CPU write unmarks only the pages it writes.
  if (scaled_resolve_extents_.size() >= kMaxScaledResolveExtents) {
    scaled_resolve_extents_.clear();
  }
  // Resolves at one base with different extents, like a downsampling chain,
  // keep the largest.
  auto extent = scaled_resolve_extents_.try_emplace(page_first, page_last);
  extent.first->second = std::max(extent.first->second, page_last);
}

uint32_t TextureCache::GuestToHostSwizzle(uint32_t guest_swizzle,
                                          uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t guest_swizzle_component = (guest_swizzle >> (3 * i)) & 0b111;
    uint32_t host_swizzle_component;
    if (guest_swizzle_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
      // Get rid of 6 and 7 values (to prevent host GPU errors if the game has
      // something broken) the simple way - by changing them to 4 (0) and 5 (1).
      host_swizzle_component = guest_swizzle_component & 0b101;
    } else {
      host_swizzle_component =
          (host_format_swizzle >> (3 * guest_swizzle_component)) & 0b111;
    }
    host_swizzle |= host_swizzle_component << (3 * i);
  }
  return host_swizzle;
}

void TextureCache::RequestTextures(uint32_t used_texture_mask) {
  const auto& regs = register_file();

  // Clear the aggregate flag, but invalidate only actually used outdated
  // bindings below to avoid resyncing all slots on unrelated texture updates.
  {
    SCOPE_profile_cpu_i("gpu", "TextureCache::InvalidateOutdatedBindings");
    texture_became_outdated_.exchange(false, std::memory_order_acquire);
    InvalidateUsedOutdatedBindings(used_texture_mask);
  }

  // Update the texture keys and the textures.
  uint32_t bindings_changed = 0;
  uint32_t textures_remaining = used_texture_mask & ~texture_bindings_in_sync_;
  uint32_t index = 0;

  Texture* textures_to_load[64];  // max bits = 32, can be unsigned + signed
                                  // means max array size = 64
  uint32_t num_textures_to_load = 0;
  {
    SCOPE_profile_cpu_i("gpu", "TextureCache::UpdateBindingKeys");
    while (xe::bit_scan_forward(textures_remaining, &index)) {
      uint32_t index_bit = UINT32_C(1) << index;
      textures_remaining = xe::clear_lowest_bit(textures_remaining);
      TextureBinding& binding = texture_bindings_[index];
      xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(index);
      TextureKey old_key = binding.key;
      uint32_t old_integer_scale_bits = binding.integer_scale_bits;
      uint8_t old_swizzled_signs = binding.swizzled_signs;
      const bool binding_was_outdated =
          old_key.is_valid && IsBindingOutdatedForUse(binding);
      BindingInfoFromFetchConstant(fetch, binding.key, &binding.swizzled_signs);
      texture_bindings_in_sync_ |= index_bit;
      if (!binding.key.is_valid) {
        if (old_key.is_valid) {
          bindings_changed |= index_bit;
        }
        binding.Reset();
        continue;
      }
      uint32_t old_host_swizzle = binding.host_swizzle;
      binding.host_swizzle =
          GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(binding.key));
      binding.integer_scale_bits =
          GetIntegerScaleBits(fetch.format, fetch.num_format, fetch.swizzle,
                              binding.swizzled_signs);

      // Check if need to load the unsigned and the signed versions of the
      // texture (if the format is emulated with different host bit
      // representations for signed and unsigned - otherwise only the unsigned
      // one is loaded).
      bool key_changed = binding.key != old_key;
      bool any_sign_was_not_signed =
          texture_util::IsAnySignNotSigned(old_swizzled_signs);
      bool any_sign_was_signed =
          texture_util::IsAnySignSigned(old_swizzled_signs);
      bool any_sign_is_not_signed =
          texture_util::IsAnySignNotSigned(binding.swizzled_signs);
      bool any_sign_is_signed =
          texture_util::IsAnySignSigned(binding.swizzled_signs);
      if (key_changed || binding.integer_scale_bits != old_integer_scale_bits ||
          binding.host_swizzle != old_host_swizzle ||
          any_sign_is_not_signed != any_sign_was_not_signed ||
          any_sign_is_signed != any_sign_was_signed) {
        bindings_changed |= index_bit;
      }
      bool load_unsigned_data = false, load_signed_data = false;
      if (IsSignedVersionSeparateForFormat(binding.key)) {
        // Can reuse previously loaded unsigned/signed versions if the key is
        // the same and the texture was previously bound as unsigned/signed
        // respectively (checking the previous values of signedness rather than
        // binding.texture != nullptr and binding.texture_signed != nullptr also
        // prevents repeated attempts to load the texture if it has failed to
        // load).
        if (any_sign_is_not_signed) {
          if (key_changed || !any_sign_was_not_signed) {
            binding.texture = FindOrCreateTexture(binding.key);
            load_unsigned_data = true;
          } else if (binding_was_outdated && binding.texture != nullptr) {
            // Fetch constants unchanged but watched guest memory changed -
            // force a data upload against the same Texture*.
            load_unsigned_data = true;
          }
        } else {
          binding.texture = nullptr;
        }
        if (any_sign_is_signed) {
          if (key_changed || !any_sign_was_signed) {
            TextureKey signed_key = binding.key;
            signed_key.signed_separate = 1;
            binding.texture_signed = FindOrCreateTexture(signed_key);
            load_signed_data = true;
          } else if (binding_was_outdated &&
                     binding.texture_signed != nullptr) {
            load_signed_data = true;
          }
        } else {
          binding.texture_signed = nullptr;
        }
      } else {
        // Same resource for both unsigned and signed, but descriptor formats
        // may be different.
        if (key_changed) {
          binding.texture = FindOrCreateTexture(binding.key);
          load_unsigned_data = true;
        } else if (binding_was_outdated && binding.texture != nullptr) {
          load_unsigned_data = true;
        }
        binding.texture_signed = nullptr;
      }
      if (load_unsigned_data && binding.texture != nullptr) {
        textures_to_load[num_textures_to_load++] = binding.texture;
      }
      if (load_signed_data && binding.texture_signed != nullptr) {
        textures_to_load[num_textures_to_load++] = binding.texture_signed;
      }
    }
  }

  LoadTexturesData(textures_to_load, num_textures_to_load);

  // TEX-1: a texture bound before any successful load samples undefined
  // contents. Report it (probe), or bind the null image until it loads.
  if (cvars::xdroid_tex_probe || cvars::xdroid_tex_unloaded_null ||
      unloaded_null_slots_) {
    uint32_t used_remaining = used_texture_mask;
    while (xe::bit_scan_forward(used_remaining, &index)) {
      uint32_t index_bit = UINT32_C(1) << index;
      used_remaining = xe::clear_lowest_bit(used_remaining);
      const TextureBinding& binding = texture_bindings_[index];
      bool hidden = false;
      for (Texture* texture : {binding.texture, binding.texture_signed}) {
        if (!texture || texture->ever_loaded()) {
          continue;
        }
        if (cvars::xdroid_tex_probe && !texture->probe_.reported_unloaded) {
          texture->probe_.reported_unloaded = true;
          ++probe_counters_.bound_unloaded;
          ProbeLog("bound_unloaded", texture->key(),
                   cvars::xdroid_tex_unloaded_null
                       ? "reason=bound before any load (null image bound)"
                       : "reason=bound before any load (undefined contents "
                         "sampled)");
        }
        hidden |= IsTextureHiddenAsUnloaded(texture);
      }
      // Rebind when a slot enters or leaves the null image.
      if (hidden != bool(unloaded_null_slots_ & index_bit)) {
        unloaded_null_slots_ ^= index_bit;
        bindings_changed |= index_bit;
      }
    }
  }

  if (bindings_changed) {
    SCOPE_profile_cpu_i("gpu", "TextureCache::UpdateTextureBindings");
    UpdateTextureBindingsImpl(bindings_changed);
  }

  RecordUsedTexturesInTrace(used_texture_mask);
}

void TextureCache::RecordUsedTexturesInTrace(uint32_t used_texture_mask) {
  SCOPE_profile_cpu_f("gpu");
  if (!trace_writer_ || !trace_writer_->is_open()) {
    return;
  }
  // Only ranges the shared memory actually uploads are recorded, so a texture
  // still resident from an earlier frame would never reach the trace. The
  // writer drops ranges it has already written.
  uint32_t textures_remaining = used_texture_mask;
  uint32_t index = 0;
  while (xe::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining = xe::clear_lowest_bit(textures_remaining);
    const TextureBinding& binding = texture_bindings_[index];
    if (!binding.key.is_valid) {
      continue;
    }
    const Texture* texture =
        binding.texture ? binding.texture : binding.texture_signed;
    if (!texture) {
      continue;
    }
    if (binding.key.base_page) {
      trace_writer_->WriteMemoryReadCached(
          binding.key.base_page << 12,
          xe::align(texture->GetGuestBaseSize(), UINT32_C(16)));
    }
    if (binding.key.mip_page) {
      trace_writer_->WriteMemoryReadCached(
          binding.key.mip_page << 12,
          xe::align(texture->GetGuestMipsSize(), UINT32_C(16)));
    }
  }
}

bool TextureCache::AnyUsedTextureRequestWorkPending(
    uint32_t used_texture_mask) const {
  if (!used_texture_mask) {
    return false;
  }
  // Any used slot that is out of sync needs work.
  if (used_texture_mask & ~texture_bindings_in_sync_) {
    return true;
  }
  // Any in-sync slot whose backing texture data is outdated also needs work.
  uint32_t used_in_sync = used_texture_mask & texture_bindings_in_sync_;
  uint32_t index = 0;
  while (xe::bit_scan_forward(used_in_sync, &index)) {
    used_in_sync = xe::clear_lowest_bit(used_in_sync);
    const TextureBinding& binding = texture_bindings_[index];
    if (binding.key.is_valid && IsBindingOutdatedForUse(binding)) {
      return true;
    }
  }
  return false;
}

bool TextureCache::IsBindingOutdatedForUse(
    const TextureBinding& binding) const {
  auto is_texture_outdated = [](const Texture* texture) {
    return texture && (texture->base_outdated_lockless() ||
                       texture->mips_outdated_lockless());
  };
  return is_texture_outdated(binding.texture) ||
         is_texture_outdated(binding.texture_signed);
}

// Clears the in-sync bit for used slots whose backing texture data is stale so
// the main RequestTextures loop reprocesses them (re-binding the same Texture*
// with fresh data uploaded via LoadTexturesData). Only used-and-outdated slots
// are touched; non-used and clean slots keep their cached state.
void TextureCache::InvalidateUsedOutdatedBindings(uint32_t used_texture_mask) {
  uint32_t used_in_sync = used_texture_mask & texture_bindings_in_sync_;
  uint32_t index = 0;
  while (xe::bit_scan_forward(used_in_sync, &index)) {
    uint32_t index_bit = UINT32_C(1) << index;
    used_in_sync = xe::clear_lowest_bit(used_in_sync);
    const TextureBinding& binding = texture_bindings_[index];
    if (IsBindingOutdatedForUse(binding)) {
      texture_bindings_in_sync_ &= ~index_bit;
    }
  }
}

const char* TextureCache::TextureKey::GetLogDimensionName(
    xenos::DataDimension dimension) {
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return "1D";
    case xenos::DataDimension::k2DOrStacked:
      return "2D";
    case xenos::DataDimension::k3D:
      return "3D";
    case xenos::DataDimension::kCube:
      return "cube";
    default:
      assert_unhandled_case(dimension);
      return "unknown";
  }
}

void TextureCache::TextureKey::LogAction(const char* action) const {
  XELOGGPU(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}), mips at 0x{:08X}",
      action, tiled ? "tiled" : "linear", scaled_resolve ? "scaled " : "",
      GetWidth(), GetHeight(), GetDepthOrArraySize(), GetLogDimensionName(),
      FormatInfo::GetName(format), mip_max_level + 1, packed_mips ? "" : "un",
      mip_max_level != 0 ? "s" : "", base_page << 12, pitch << 5,
      mip_page << 12);
}

void TextureCache::Texture::LogAction(const char* action) const {
  XELOGGPU(
      "{} {} {}{}x{}x{} {} {} texture with {} {}packed mip level{}, "
      "base at 0x{:08X} (pitch {}, size 0x{:08X}), mips at 0x{:08X} (size "
      "0x{:08X})",
      action, key_.tiled ? "tiled" : "linear",
      key_.scaled_resolve ? "scaled " : "", key_.GetWidth(), key_.GetHeight(),
      key_.GetDepthOrArraySize(), key_.GetLogDimensionName(),
      FormatInfo::GetName(key_.format), key_.mip_max_level + 1,
      key_.packed_mips ? "" : "un", key_.mip_max_level != 0 ? "s" : "",
      key_.base_page << 12, key_.pitch << 5, GetGuestBaseSize(),
      key_.mip_page << 12, GetGuestMipsSize());
}

// The texture must be in the recent usage list. Place it in front now because
// after creation, the texture will likely be used immediately, and it should
// not be destroyed immediately after creation if dropping of old textures is
// performed somehow. The list is maintained by the Texture, not the
// TextureCache itself (unlike the `textures_` container).
TextureCache::Texture::Texture(TextureCache& texture_cache,
                               const TextureKey& key, bool track_usage)
    : texture_cache_(texture_cache),
      key_(key),
      guest_layout_(key.GetGuestLayout()),
      last_usage_submission_index_(texture_cache.current_submission_index_),
      last_usage_time_(texture_cache.current_submission_time_),
      used_previous_(track_usage ? texture_cache.texture_used_last_ : nullptr),
      used_next_(nullptr),
      in_usage_list_(track_usage) {
  if (track_usage) {
    if (texture_cache.texture_used_last_) {
      texture_cache.texture_used_last_->used_next_ = this;
    } else {
      texture_cache.texture_used_first_ = this;
    }
    texture_cache.texture_used_last_ = this;
  }

  // Never try to upload data that doesn't exist.
  base_outdated_ = guest_layout().base.level_data_extent_bytes != 0;
  mips_outdated_ = guest_layout().mips_total_extent_bytes != 0;
}

TextureCache::Texture::~Texture() {
  auto link_it = texture_cache_.link_candidates_.find(key_.base_page);
  if (link_it != texture_cache_.link_candidates_.end() &&
      link_it->second == this) {
    texture_cache_.link_candidates_.erase(link_it);
  }
  if (mips_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(mips_watch_handle_);
  }
  if (base_watch_handle_) {
    texture_cache().shared_memory().UnwatchMemoryRange(base_watch_handle_);
  }

  // Only remove from usage list if we were added to it (track_usage=true).
  if (in_usage_list_) {
    if (used_previous_) {
      used_previous_->used_next_ = used_next_;
    } else {
      texture_cache_.texture_used_first_ = used_next_;
    }
    if (used_next_) {
      used_next_->used_previous_ = used_previous_;
    } else {
      texture_cache_.texture_used_last_ = used_previous_;
    }
  }

  texture_cache_.UpdateTexturesTotalHostMemoryUsage(0, host_memory_usage_);
}

bool TextureCache::Texture::MakeUpToDateAndWatch(
    const global_unique_lock_type& global_lock) {
  SharedMemory& shared_memory = texture_cache().shared_memory();
  const bool watch_base = base_outdated_;
  const bool watch_mips = mips_outdated_;
  assert_true(global_lock.owns_lock());
  if (watch_base &&
      !shared_memory.IsRangeValid(
          key().base_page << 12, xe::align(GetGuestBaseSize(), UINT32_C(16)))) {
    return false;
  }
  if (watch_mips &&
      !shared_memory.IsRangeValid(
          key().mip_page << 12, xe::align(GetGuestMipsSize(), UINT32_C(16)))) {
    return false;
  }

  if (watch_base) {
    assert_not_zero(GetGuestBaseSize());
    base_outdated_ = false;
    base_watch_handle_ = shared_memory.WatchMemoryRange(
        key().base_page << 12, GetGuestBaseSize(), TextureCache::WatchCallback,
        this, nullptr, 0);
  }
  if (watch_mips) {
    assert_not_zero(GetGuestMipsSize());
    mips_outdated_ = false;
    mips_watch_handle_ = shared_memory.WatchMemoryRange(
        key().mip_page << 12, GetGuestMipsSize(), TextureCache::WatchCallback,
        this, nullptr, 1);
  }
  return true;
}

void TextureCache::Texture::MarkAsUsed() {
  // Textures not in usage tracking (track_usage=false) should not be linked.
  if (!in_usage_list_) {
    return;
  }
  assert_true(last_usage_submission_index_ <=
              texture_cache_.current_submission_index_);
  // This is called very frequently, don't relink unless needed for caching.
  if (last_usage_submission_index_ >=
      texture_cache_.current_submission_index_) {
    return;
  }
  last_usage_submission_index_ = texture_cache_.current_submission_index_;
  last_usage_time_ = texture_cache_.current_submission_time_;
  if (used_next_ == nullptr) {
    // Already the most recently used.
    return;
  }
  if (used_previous_ != nullptr) {
    used_previous_->used_next_ = used_next_;
  } else {
    texture_cache_.texture_used_first_ = used_next_;
  }
  used_next_->used_previous_ = used_previous_;
  used_previous_ = texture_cache_.texture_used_last_;
  used_next_ = nullptr;
  texture_cache_.texture_used_last_->used_next_ = this;
  texture_cache_.texture_used_last_ = this;
}

void TextureCache::Texture::WatchCallback(
    [[maybe_unused]] const global_unique_lock_type& global_lock, bool is_mip) {
  if (is_mip) {
    assert_not_zero(GetGuestMipsSize());
    mips_outdated_ = true;
    mips_watch_handle_ = nullptr;
  } else {
    assert_not_zero(GetGuestBaseSize());
    base_outdated_ = true;
    base_watch_handle_ = nullptr;
  }
}

void TextureCache::WatchCallback(const global_unique_lock_type& global_lock,
                                 void* context, void* data, uint64_t argument,
                                 bool invalidated_by_gpu) {
  Texture& texture = *static_cast<Texture*>(context);
  texture.WatchCallback(global_lock, argument != 0);
  if (cvars::xdroid_tex_census) {
    texture.census_.inval |=
        uint8_t((invalidated_by_gpu ? 2 : 1) << (argument != 0 ? 2 : 0));
  }
  if (cvars::xdroid_tex_probe && texture.ever_loaded() &&
      texture.texture_cache().probe_frame() -
              texture.probe_.first_load_frame <=
          4) {
    texture.probe_.early_invalidation |= invalidated_by_gpu ? 2 : 1;
  }
  texture.texture_cache().texture_became_outdated_.store(
      true, std::memory_order_release);
}

void TextureCache::DestroyAllTextures(bool from_destructor) {
  ResetTextureBindings(from_destructor);
  textures_.clear();
  COUNT_profile_set("gpu/texture_cache/textures", 0);
}

TextureCache::Texture* TextureCache::FindOrCreateTexture(TextureKey key) {
  SCOPE_profile_cpu_f("gpu");
  // Check if the texture is a scaled resolve texture.
  if (IsDrawResolutionScaled() && key.tiled &&
      IsScaledResolveSupportedForFormat(key)) {
    texture_util::TextureGuestLayout scaled_resolve_guest_layout =
        key.GetGuestLayout();
    if ((scaled_resolve_guest_layout.base.level_data_extent_bytes &&
         IsRangeScaledResolved(
             key.base_page << 12,
             scaled_resolve_guest_layout.base.level_data_extent_bytes)) ||
        (scaled_resolve_guest_layout.mips_total_extent_bytes &&
         IsRangeScaledResolved(
             key.mip_page << 12,
             scaled_resolve_guest_layout.mips_total_extent_bytes))) {
      key.scaled_resolve = 1;
    }
  }

  uint32_t host_width = key.GetWidth();
  uint32_t host_height = key.GetHeight();
  if (key.scaled_resolve) {
    host_width *= draw_resolution_scale_x();
    host_height *= draw_resolution_scale_y();
  }
  // With 3x resolution scaling, a 2D texture may become bigger than the
  // Direct3D 11 limit, and with 2x, a 3D one as well.
  // TODO(Triang3l): Skip mips on Vulkan in this case - the minimum requirement
  // there is 4096, which is below the Xenos maximum texture size of 8192.
  uint32_t max_host_width_height = GetMaxHostTextureWidthHeight(key.dimension);
  uint32_t max_host_depth_or_array_size =
      GetMaxHostTextureDepthOrArraySize(key.dimension);
  if (host_width > max_host_width_height ||
      host_height > max_host_width_height ||
      key.GetDepthOrArraySize() > max_host_depth_or_array_size) {
    if (cvars::xdroid_tex_probe) {
      ++probe_counters_.create_fail;
      ProbeLog("create_fail", key, "reason=over host size limit");
    }
    return nullptr;
  }

  // Try to find an existing texture.
  // TODO(Triang3l): Reuse a texture with mip_page unchanged, but base_page
  // previously 0, now not 0, to save memory - common case in streaming.
  auto found_texture_it = textures_.find(key);
  if (found_texture_it != textures_.end()) {
    return found_texture_it->second.get();
  }

  // Create the texture and add it to the map.
  Texture* texture;
  {
    std::unique_ptr<Texture> new_texture = CreateTexture(key);
    if (!new_texture) {
      key.LogAction("Failed to create");
      if (cvars::xdroid_tex_probe) {
        ++probe_counters_.create_fail;
        ProbeLog("create_fail", key,
                 "reason=no host format or image allocation failed");
      }
      return nullptr;
    }
    assert_true(new_texture->key() == key);
    texture =
        textures_.emplace(key, std::move(new_texture)).first->second.get();
  }
  COUNT_profile_set("gpu/texture_cache/textures", textures_.size());
  texture->LogAction("Created");
  if (cvars::xdroid_tex_probe) {
    ++probe_counters_.created;
    auto evicted_it = probe_evicted_.find(key);
    if (evicted_it != probe_evicted_.end()) {
      uint64_t frames_since = probe_frame() - evicted_it->second;
      probe_evicted_.erase(evicted_it);
      if (frames_since <= 900) {
        ++probe_counters_.evict_recreate;
        ProbeLog("evict_recreate", key,
                 fmt::format("reason=budget evicted {} frames ago mem_mb={}",
                             frames_since,
                             (textures_total_host_memory_usage_ +
                              ((UINT32_C(1) << 20) - 1)) >>
                                 20)
                     .c_str());
      }
    }
  }
  return texture;
}

// Packs the integer scale the fetch shader reads from the system constant to
// undo the host sampler's normalization - the guest wants e.g. [0, 255], not
// [0, 1]. 6 bits per output component: bits 0:3 = width - 1, bit 4 = signed,
// bit 5 = unsigned-biased. The scale lands after swizzling, so each output lane
// walks the guest swizzle back to its source component's width. component_bits
// only describes the stored width, so the source component is clamped to the
// last stored channel the same way the host swizzle expands the formats.
// (k_16 has a 16 bit width in all four components, k_5_6_5 gives blue in W.)
// Constant (0/1) lanes, gamma, and non-fixed formats have nothing to rescale
// and stay 0. Bit 24 for normalized fixed fetches.
uint32_t TextureCache::GetIntegerScaleBits(xenos::TextureFormat guest_format,
                                           uint32_t num_format,
                                           uint32_t guest_swizzle,
                                           uint8_t swizzled_signs) {
  const FormatInfo& format_info = *FormatInfo::Get(guest_format);
  uint32_t scale_bits = 0;

  if (!format_info.fixed) {
    return 0;
  }

  if (!num_format) {
    return swizzled_signs == kSwizzledSignsUnsigned ? UINT32_C(1) << 24 : 0;
  }

  uint32_t last_stored_component = 0;
  for (uint32_t i = 1; i < 4; ++i) {
    if (format_info.component_bits[i]) {
      last_stored_component = i;
    }
  }

  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t source_component = (guest_swizzle >> (i * 3)) & 0b111;
    if (source_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
      continue;
    }
    source_component = std::min(source_component, last_stored_component);

    xenos::TextureSign sign =
        xenos::TextureSign((swizzled_signs >> (i * 2)) & 0b11);

    uint8_t width = format_info.component_bits[source_component];
    if (!width || width > 16 || sign == xenos::TextureSign::kGamma) {
      continue;
    }

    uint32_t component_scale = uint32_t(width - 1);
    if (sign == xenos::TextureSign::kSigned) {
      component_scale |= UINT32_C(1) << 4;
      // Unsigned-biased: halve the scaled value and apply an extra offset.
    } else if (sign == xenos::TextureSign::kUnsignedBiased) {
      component_scale |= UINT32_C(1) << 5;
    }

    scale_bits |= component_scale << (i * 6);
  }

  return scale_bits;
}

void TextureCache::LoadTexturesData(Texture** textures, uint32_t n_textures) {
  SCOPE_profile_cpu_f("gpu");
  assert_true(n_textures <= 64);
  if (n_textures < 2) {
    if (!n_textures) {
      return;
    } else {
      LoadTextureData(*textures[0]);
      return;
    }
  }

  // Lockless pre-check: count how many textures appear outdated.
  // If none appear outdated, skip the lock entirely.
  uint32_t likely_outdated = 0;
  for (uint32_t i = 0; i < n_textures; ++i) {
    Texture* current = textures[i];
    if (current->base_outdated_lockless() ||
        current->mips_outdated_lockless()) {
      ++likely_outdated;
    }
  }
  if (likely_outdated == 0) {
    // All textures appear up-to-date, skip lock acquisition
    return;
  }

  uint64_t index_base_outdated = 0;
  uint64_t index_mips_outdated = 0;
  uint32_t nkept = 0;
  {
    SCOPE_profile_cpu_i("gpu", "TextureCache::OutdatedScanUnderLock");
    auto global_lock = global_critical_region_.Acquire();
    for (uint32_t i = 0; i < n_textures; ++i) {
      Texture* current = textures[i];

      auto base_outdated = current->base_outdated(global_lock);
      auto mips_outdated = current->mips_outdated(global_lock);

      index_base_outdated |= static_cast<uint64_t>(base_outdated) << i;
      index_mips_outdated |= static_cast<uint64_t>(mips_outdated) << i;
      if (!base_outdated && !mips_outdated) {
        textures[i] = nullptr;

      } else {
        nkept++;
      }
    }
  }

  if (nkept == 0) {
    return;
  }

  for (uint32_t i = 0; i < n_textures; ++i) {
    Texture* p_texture = textures[i];
    if (!p_texture) {
      continue;
    }
    textures[i] = nullptr;
    Texture& texture = *p_texture;

    TextureKey texture_key = texture.key();
    // Implementation may load multiple blocks at once via accesses of up to 128
    // bits (R32G32B32A32_UINT), so aligning the size to this value to make sure
    // if the texture is small (especially if it's linear), the last blocks
    // won't be cut off (hosts may return 0, 0, 0, 0 for the whole
    // R32G32B32A32_UINT access for the non-16-aligned tail even if 1...15 bytes
    // are actually provided for it).

    // Request uploading of the texture data to the shared memory.
    // This is also necessary when resolution scaling is used - the texture
    // cache relies on shared memory for invalidation of both unscaled and
    // scaled textures. Plus a texture may be unscaled partially, when only a
    // portion of its pages is invalidated, in this case we'll need the texture
    // from the shared memory to load the unscaled parts.
    // TODO(Triang3l): Load unscaled parts.
    {
      SCOPE_profile_cpu_i("gpu", "TextureCache::RequestGuestRanges");
      if (index_base_outdated & (1ULL << i)) {
        if (!shared_memory().RequestRange(
                texture_key.base_page << 12,
                xe::align(texture.GetGuestBaseSize(), UINT32_C(16)))) {
          ProbeLoadResult(texture, "shared memory upload of the base failed");
          continue;
        }
      }
      if (index_mips_outdated & (1ULL << i)) {
        if (!shared_memory().RequestRange(
                texture_key.mip_page << 12,
                xe::align(texture.GetGuestMipsSize(), UINT32_C(16)))) {
          ProbeLoadResult(texture, "shared memory upload of the mips failed");
          continue;
        }
      }
    }
    if (texture_key.scaled_resolve) {
      // Make sure all the scaled resolve memory is resident and accessible from
      // the shader, including any possible padding that hasn't yet been touched
      // by an actual resolve, but is still included in the texture size, so the
      // GPU won't be trying to access unmapped memory.
      if (!EnsureScaledResolveMemoryCommitted(texture_key.base_page << 12,
                                              texture.GetGuestBaseSize(), 4) ||
          !EnsureScaledResolveMemoryCommitted(texture_key.mip_page << 12,
                                              texture.GetGuestMipsSize(), 4)) {
        ProbeLoadResult(texture, "scaled resolve memory commit failed");
        continue;
      }
    }

    // Actually load the texture data.
    load_fail_reason_ = nullptr;
    if (!LoadTextureDataFromResidentMemoryImpl(
            texture, (index_base_outdated & (1ULL << i)) != 0,
            (index_mips_outdated & (1ULL << i)) != 0)) {
      ProbeLoadResult(texture,
                      load_fail_reason_ ? load_fail_reason_ : "load failed");
      continue;
    }
    ProbeLoadResult(texture, nullptr);

    // reque for makeuptodatandwatch
    textures[i] = &texture;
  }
  {
    auto crit = global_critical_region_.Acquire();

    for (uint32_t i = 0; i < n_textures; ++i) {
      auto texture = textures[i];
      if (!texture) {
        continue;
      }
      // Mark the ranges as uploaded and watch them. This is needed for scaled
      // resolves as well to detect when the CPU wants to reuse the memory for a
      // regular texture or a vertex buffer, and thus the scaled resolve version
      // is not up to date anymore.
      if (!texture->MakeUpToDateAndWatch(crit)) {
        if (cvars::xdroid_tex_probe) {
          ++probe_counters_.unwatched;
          ProbeLog("unwatched", texture->key(),
                   "reason=guest memory rewritten during the load (torn "
                   "data until the next use reloads it)");
        }
        continue;
      }

      texture->LogAction("Loaded");
    }
  }
}
bool TextureCache::LoadTextureData(Texture& texture) {
  SCOPE_profile_cpu_f("gpu");
  // Lockless pre-check: if texture appears up-to-date, skip the lock.
  // This is safe because worst case is a false positive (we acquire lock
  // unnecessarily), never a false negative.
  if (!texture.base_outdated_lockless() && !texture.mips_outdated_lockless()) {
    return true;
  }

  // Check what needs to be uploaded.
  bool base_outdated, mips_outdated;
  uint8_t census_inval;
  {
    auto global_lock = global_critical_region_.Acquire();
    base_outdated = texture.base_outdated(global_lock);
    mips_outdated = texture.mips_outdated(global_lock);
    census_inval = texture.census_.inval;
    texture.census_.inval = 0;
  }
  if (!base_outdated && !mips_outdated) {
    return true;
  }

  TextureKey texture_key = texture.key();

  // Implementation may load multiple blocks at once via accesses of up to 128
  // bits (R32G32B32A32_UINT), so aligning the size to this value to make sure
  // if the texture is small (especially if it's linear), the last blocks won't
  // be cut off (hosts may return 0, 0, 0, 0 for the whole R32G32B32A32_UINT
  // access for the non-16-aligned tail even if 1...15 bytes are actually
  // provided for it).

  // Request uploading of the texture data to the shared memory.
  // This is also necessary when resolution scaling is used - the texture cache
  // relies on shared memory for invalidation of both unscaled and scaled
  // textures. Plus a texture may be unscaled partially, when only a portion of
  // its pages is invalidated, in this case we'll need the texture from the
  // shared memory to load the unscaled parts.
  // TODO(Triang3l): Load unscaled parts.
  if (base_outdated) {
    if (!shared_memory().RequestRange(
            texture_key.base_page << 12,
            xe::align(texture.GetGuestBaseSize(), UINT32_C(16)))) {
      ProbeLoadResult(texture, "shared memory upload of the base failed");
      return false;
    }
  }
  if (mips_outdated) {
    if (!shared_memory().RequestRange(
            texture_key.mip_page << 12,
            xe::align(texture.GetGuestMipsSize(), UINT32_C(16)))) {
      ProbeLoadResult(texture, "shared memory upload of the mips failed");
      return false;
    }
  }
  if (texture_key.scaled_resolve) {
    // Make sure all the scaled resolve memory is resident and accessible from
    // the shader, including any possible padding that hasn't yet been touched
    // by an actual resolve, but is still included in the texture size, so the
    // GPU won't be trying to access unmapped memory.
    if (!EnsureScaledResolveMemoryCommitted(texture_key.base_page << 12,
                                            texture.GetGuestBaseSize(), 4) ||
        !EnsureScaledResolveMemoryCommitted(texture_key.mip_page << 12,
                                            texture.GetGuestMipsSize(), 4)) {
      ProbeLoadResult(texture, "scaled resolve memory commit failed");
      return false;
    }
  }

  // TEX-1a: hash the CPU-written guest bytes of the outdated parts after
  // RequestRange, so the hash matches what was uploaded: a CPU write after it
  // invalidates the range, MakeUpToDateAndWatch then fails and the hash is
  // dropped. A part whose bytes equal the last loaded ones (the watch fired on
  // a write that changed nothing in it, or on other data sharing its pages) is
  // already in the host texture: xdroid_tex_skip_same skips loading it.
  const bool census = cvars::xdroid_tex_census;
  const bool skip_same = cvars::xdroid_tex_skip_same;
  std::chrono::steady_clock::time_point census_start;
  if (census) {
    census_start = std::chrono::steady_clock::now();
  }
  uint64_t base_hash = 0, mips_hash = 0;
  bool base_same = false, mips_same = false;
  if ((census || skip_same) && !texture_key.scaled_resolve) {
    if (base_outdated) {
      uint32_t start = texture_key.base_page << 12;
      uint32_t size = texture.GetGuestBaseSize();
      if (!shared_memory().IsRangeAnyGpuWritten(start, size)) {
        base_hash = shared_memory().HashRange(start, size);
      }
      base_same = texture.ever_loaded() && base_hash &&
                  base_hash == texture.census_.base_hash;
    }
    if (mips_outdated) {
      uint32_t start = texture_key.mip_page << 12;
      uint32_t size = texture.GetGuestMipsSize();
      if (!shared_memory().IsRangeAnyGpuWritten(start, size)) {
        mips_hash = shared_memory().HashRange(start, size);
      }
      mips_same = texture.ever_loaded() && mips_hash &&
                  mips_hash == texture.census_.mips_hash;
    }
  }
  const bool census_same =
      (!base_outdated || base_same) && (!mips_outdated || mips_same);
  bool load_base = base_outdated && !(skip_same && base_same);
  bool load_mips = mips_outdated && !(skip_same && mips_same);

  // Actually load the texture data.
  if (load_base || load_mips) {
    load_fail_reason_ = nullptr;
    if (!LoadTextureDataFromResidentMemoryImpl(texture, load_base,
                                               load_mips)) {
      ProbeLoadResult(texture,
                      load_fail_reason_ ? load_fail_reason_ : "load failed");
      return false;
    }
  }
  if (census) {
    CensusRecord(
        texture, census_inval, base_outdated, mips_outdated, census_same,
        !load_base && !load_mips,
        uint64_t(load_base ? texture.GetGuestBaseSize() : 0) +
            (load_mips ? texture.GetGuestMipsSize() : 0),
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - census_start)
                     .count()));
  }
  ProbeLoadResult(texture, nullptr);

  // Mark the ranges as uploaded and watch them. This is needed for scaled
  // resolves as well to detect when the CPU wants to reuse the memory for a
  // regular texture or a vertex buffer, and thus the scaled resolve version is
  // not up to date anymore.
  if (!texture.MakeUpToDateAndWatch(global_critical_region_.Acquire())) {
    // Rewritten since RequestRange: the hashes may not match the loaded data.
    texture.census_.base_hash = 0;
    texture.census_.mips_hash = 0;
    if (cvars::xdroid_tex_probe) {
      ++probe_counters_.unwatched;
      ProbeLog("unwatched", texture.key(),
               "reason=guest memory rewritten during the load (torn data "
               "until the next use reloads it)");
    }
    return false;
  }
  if (base_outdated) {
    texture.census_.base_hash = base_hash;
    // TEX-1b: one 2D tiled level - what a resolve can write whole.
    if (load_base && texture_key.dimension == xenos::DataDimension::k2DOrStacked &&
        !texture_key.depth_or_array_size_minus_1 && texture_key.tiled &&
        !texture_key.scaled_resolve && !texture_key.mip_max_level &&
        !texture_key.packed_mips) {
      link_candidates_[texture_key.base_page] = &texture;
    }
  }
  if (mips_outdated) {
    texture.census_.mips_hash = mips_hash;
  }

  texture.LogAction("Loaded");

  return true;
}

void TextureCache::BindingInfoFromFetchConstant(
    const xenos::xe_gpu_texture_fetch_t& fetch, TextureKey& key_out,
    uint8_t* swizzled_signs_out) {
  // Reset the key and the signedness.
  key_out.MakeInvalid();
  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out =
        uint8_t(xenos::TextureSign::kUnsigned) * uint8_t(0b01010101);
  }

  switch (fetch.type) {
    case xenos::FetchConstantType::kTexture:
      break;
    case xenos::FetchConstantType::kInvalidTexture:
      if (cvars::gpu_allow_invalid_fetch_constants) {
        break;
      }
      XELOGW(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "has \"invalid\" type! This is incorrect behavior, but you can try "
          "bypassing this by launching Xenia with "
          "--gpu_allow_invalid_fetch_constants=true.",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3,
          fetch.dword_4, fetch.dword_5);
      return;
    default:
      XELOGW(
          "Texture fetch constant ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) "
          "is completely invalid!",
          fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3,
          fetch.dword_4, fetch.dword_5);
      return;
  }

  uint32_t width_minus_1, height_minus_1, depth_or_array_size_minus_1;
  uint32_t base_page, mip_page, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(
      fetch, &width_minus_1, &height_minus_1, &depth_or_array_size_minus_1,
      &base_page, &mip_page, nullptr, &mip_max_level);
  if (base_page == 0 && mip_page == 0 &&
      (fetch.swizzle & 0b110110110110) != 0b100100100100) {
    // No texture data at all. Any header taking every swizzle component from
    // literal 0s or 1s may still be valid. 4D530919 binds one as the dummy
    // alpha plane of Bink movies. Such examples get a texture with their
    // dimensions for LOD queries. Zero extents skip the upload and watches.
    return;
  }
  uint32_t pitch = fetch.pitch;
  if (fetch.dimension == xenos::DataDimension::k1D) {
    bool is_invalid_1d = false;
    // Handle wide 1D textures (> 8192 wide) by mapping them to a 2D grid.
    // The shader will convert 1D coordinates to 2D using the original width
    // from the fetch constant.
    if (width_minus_1 >= xenos::kTexture2DCubeMaxWidthHeight) {
      uint32_t total_width = width_minus_1 + 1;
      uint32_t row_width = xenos::kTexture2DCubeMaxWidthHeight;
      uint32_t num_rows = (total_width + row_width - 1) / row_width;
      // Cap the materialized rows - huge widths are index-space declarations
      // with little real data behind them, and the full extent may run past
      // the 512 MB physical space (making the load fail entirely). Kept in
      // sync with the shader-side row cap.
      num_rows = std::min(num_rows, xenos::kTexture1DWideMaxRows);
      width_minus_1 = row_width - 1;
      height_minus_1 = num_rows - 1;
      // Disable mipmaps for wide 1D textures. The shader's coordinate remapping
      // assumes base-level dimensions (num_rows), but at mip level N, the 2D
      // texture becomes (8192 >> N) x (num_rows >> N), which breaks the mapping
      // when num_rows >> N becomes 1 while the shader still expects multiple
      // rows. Mipmaps are rarely used with 1D lookup textures anyway.
      mip_max_level = 0;
      // The guest pitch is meaningless for a texture the guest believes is 1D
      // (the 9 bit field couldn't even express the line width).
      pitch = xenos::kTexture2DCubeMaxWidthHeight >> 5;
    }
    assert_false(fetch.tiled);
    if (fetch.tiled) {
      XELOGE(
          "1D texture has tiling enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Report the game to Xenia "
          "developers");
      is_invalid_1d = true;
    }
    assert_false(fetch.packed_mips);
    if (fetch.packed_mips) {
      XELOGE(
          "1D texture has packed mips enabled in the fetch constant, but this "
          "appears to be completely wrong - ignoring! Report the game to Xenia "
          "developers");
      is_invalid_1d = true;
    }
    if (is_invalid_1d) {
      return;
    }
  }

  xenos::TextureFormat format = GetBaseFormat(fetch.format);

  key_out.base_page = base_page;
  key_out.mip_page = mip_page;
  key_out.dimension = fetch.dimension;
  key_out.width_minus_1 = width_minus_1;
  key_out.height_minus_1 = height_minus_1;
  key_out.depth_or_array_size_minus_1 = depth_or_array_size_minus_1;
  key_out.pitch = pitch;
  key_out.mip_max_level = mip_max_level;
  key_out.tiled = fetch.tiled;
  key_out.packed_mips = fetch.packed_mips;
  key_out.format = format;
  key_out.endianness = fetch.endianness;

  key_out.is_valid = 1;

  if (swizzled_signs_out != nullptr) {
    *swizzled_signs_out = texture_util::SwizzleSigns(fetch);
  }
}

void TextureCache::ResetTextureBindings(bool from_destructor) {
  uint32_t bindings_reset = 0;
  for (size_t i = 0; i < texture_bindings_.size(); ++i) {
    TextureBinding& binding = texture_bindings_[i];
    if (!binding.key.is_valid) {
      continue;
    }
    binding.Reset();
    bindings_reset |= UINT32_C(1) << i;
  }
  texture_bindings_in_sync_ &= ~bindings_reset;
  if (!from_destructor && bindings_reset) {
    UpdateTextureBindingsImpl(bindings_reset);
  }
}

void TextureCache::UpdateTexturesTotalHostMemoryUsage(uint64_t add,
                                                      uint64_t subtract) {
  textures_total_host_memory_usage_ =
      textures_total_host_memory_usage_ - subtract + add;
  COUNT_profile_set("gpu/texture_cache/total_host_memory_usage_mb",
                    uint32_t((textures_total_host_memory_usage_ +
                              ((UINT32_C(1) << 20) - 1)) >>
                             20));
}

bool TextureCache::IsRangeScaledResolved(uint32_t start_unscaled,
                                         uint32_t length_unscaled) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }

  start_unscaled = std::min(start_unscaled, SharedMemory::kBufferSize);
  length_unscaled =
      std::min(length_unscaled, SharedMemory::kBufferSize - start_unscaled);
  if (!length_unscaled) {
    return false;
  }

  // Two-level check for faster rejection since resolve targets are usually
  // placed in relatively small and localized memory portions (confirmed by
  // testing - pretty much all times the deeper level was entered, the texture
  // was a resolve target).
  uint32_t page_first = start_unscaled >> 12;
  uint32_t page_last = (start_unscaled + length_unscaled - 1) >> 12;
  uint32_t block_first = page_first >> 5;
  uint32_t block_last = page_last >> 5;
  uint32_t l2_block_first = block_first >> 6;
  uint32_t l2_block_last = block_last >> 6;
  auto global_lock = global_critical_region_.Acquire();
  for (uint32_t i = l2_block_first; i <= l2_block_last; ++i) {
    uint64_t l2_block = scaled_resolve_pages_l2_[i];
    if (i == l2_block_first) {
      l2_block &= ~((UINT64_C(1) << (block_first & 63)) - 1);
    }
    if (i == l2_block_last && (block_last & 63) != 63) {
      l2_block &= (UINT64_C(1) << ((block_last & 63) + 1)) - 1;
    }
    uint32_t block_relative_index;
    while (xe::bit_scan_forward(l2_block, &block_relative_index)) {
      l2_block &= ~(UINT64_C(1) << block_relative_index);
      uint32_t block_index = (i << 6) + block_relative_index;
      uint32_t check_bits = UINT32_MAX;
      if (block_index == block_first) {
        check_bits &= ~((UINT32_C(1) << (page_first & 31)) - 1);
      }
      if (block_index == block_last && (page_last & 31) != 31) {
        check_bits &= (UINT32_C(1) << ((page_last & 31) + 1)) - 1;
      }
      if (scaled_resolve_pages_[block_index] & check_bits) {
        return true;
      }
    }
  }
  return false;
}

void TextureCache::ScaledResolveGlobalWatchCallbackThunk(
    const global_unique_lock_type& global_lock, void* context,
    uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu) {
  TextureCache* texture_cache = reinterpret_cast<TextureCache*>(context);
  texture_cache->ScaledResolveGlobalWatchCallback(
      global_lock, address_first, address_last, invalidated_by_gpu);
}

void TextureCache::ScaledResolveGlobalWatchCallback(
    const global_unique_lock_type& global_lock, uint32_t address_first,
    uint32_t address_last, bool invalidated_by_gpu) {
  assert_true(IsDrawResolutionScaled());
  if (invalidated_by_gpu) {
    // Resolves themselves do exactly the opposite of what this should do.
    return;
  }
  // Mark scaled resolve ranges as non-scaled. Textures themselves will be
  // invalidated by their shared memory watches.
  uint32_t page_first = address_first >> 12;
  uint32_t page_last = address_last >> 12;
  if (!UnmarkScaledResolvePages(page_first, page_last)) {
    return;
  }
  // The rest of any resolve the write lands in too. A texture over it would
  // otherwise still load from the scaled data, as a range counts as scaled if
  // any page of it is, and miss what the CPU wrote.
  for (auto it = scaled_resolve_extents_.begin();
       it != scaled_resolve_extents_.end() && it->first <= page_last;) {
    if (it->second < page_first) {
      ++it;
      continue;
    }
    UnmarkScaledResolvePages(it->first, it->second);
    it = scaled_resolve_extents_.erase(it);
  }
}

bool TextureCache::UnmarkScaledResolvePages(uint32_t resolve_page_first,
                                            uint32_t resolve_page_last) {
  bool unmarked = false;
  uint32_t resolve_block_first = resolve_page_first >> 5;
  uint32_t resolve_block_last = resolve_page_last >> 5;
  uint32_t resolve_l2_block_first = resolve_block_first >> 6;
  uint32_t resolve_l2_block_last = resolve_block_last >> 6;
  for (uint32_t i = resolve_l2_block_first; i <= resolve_l2_block_last; ++i) {
    uint64_t resolve_l2_block = scaled_resolve_pages_l2_[i];
    // Pre-mask to only process blocks within the write range.
    if (i == resolve_l2_block_first) {
      resolve_l2_block &= ~((UINT64_C(1) << (resolve_block_first & 63)) - 1);
    }
    if (i == resolve_l2_block_last && (resolve_block_last & 63) != 63) {
      resolve_l2_block &= (UINT64_C(1) << ((resolve_block_last & 63) + 1)) - 1;
    }
    uint32_t resolve_block_relative_index;
    while (
        xe::bit_scan_forward(resolve_l2_block, &resolve_block_relative_index)) {
      resolve_l2_block &= ~(UINT64_C(1) << resolve_block_relative_index);
      uint32_t resolve_block_index = (i << 6) + resolve_block_relative_index;
      uint32_t resolve_keep_bits = 0;
      if (resolve_block_index == resolve_block_first) {
        resolve_keep_bits |= (UINT32_C(1) << (resolve_page_first & 31)) - 1;
      }
      if (resolve_block_index == resolve_block_last &&
          (resolve_page_last & 31) != 31) {
        resolve_keep_bits |=
            ~((UINT32_C(1) << ((resolve_page_last & 31) + 1)) - 1);
      }
      if (scaled_resolve_pages_[resolve_block_index] & ~resolve_keep_bits) {
        unmarked = true;
      }
      scaled_resolve_pages_[resolve_block_index] &= resolve_keep_bits;
      if (scaled_resolve_pages_[resolve_block_index] == 0) {
        scaled_resolve_pages_l2_[i] &=
            ~(UINT64_C(1) << resolve_block_relative_index);
      }
    }
  }
  return unmarked;
}

}  // namespace gpu
}  // namespace xe
