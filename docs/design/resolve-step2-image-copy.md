# Resolve rework step 2: image copy and partial-rectangle link

Status 2026-11-12 20:50Z: approved by the maintainer (18:40Z, "el recomendado es siempre el si"), reading done, points 4
and 8 verified (below), NOTHING coded. Line numbers as of e1db94174. Files: `V` = `src/xenia/gpu/vulkan`, `G` = `src/xenia/gpu`.

## Evidence
- Bench flight.xtr with `xdroid_tex_link_mips=5`: GPU 16.06 ms = in-pass 12.01 + between passes 4.17. Gap after
  pass by end reason: resolve 72 passes, 2.02 ms (28.1 us each); barrier 9, 1.26; texload 2, 0.62; rt 22, 0.25.
- Each resolve dispatch costs 28 to 36 us whatever its size. Arm `xdroid_resolve_skip=4` (no dispatch): 14.95 /
  15.40 against 17.01 / 17.26, so the ceiling for removing copy dispatches is about 2.0 ms (12%).
- The link's buffer-to-image copy costs 8 to 25 us. 35 of 82 resolves per frame are the seven 256x256 mip chains.
- Rejects left per frame (`XDROID-TEXLINK reject=`): 0x1B290000 1280x720 k_8_8_8_8 resolved as two 640x720
  halves (reason 4); 0x1C461000 640x360 and 0x1B628000 1280x720 come from depth sources (reasons 1 and 2: not
  linkable as a bit copy, leave them); 0x1AB60000 never reaches a counted reject (probably the averaged rule).

## Switch
`xdroid_tex_link_mips` (texture_cache.cc 90, latched per frame in `TextureCache::BeginFrame` into `link_mips_`,
texture_cache.h 491, protected: add a public getter). Existing bits 1, 2, 4. New: 8 = image copy (stage A,
default off), 16 = partial-rectangle link. Default stays 5 until the bench says otherwise. Extend the cvar text.

## Stage A: bit 8, image-to-image copy (diagnostic: shared memory left stale)
Where: `DirectResolveRenderTargets` (V/vulkan_render_target_cache.cc 4638), right after the link eligibility block
(the `if (link) { ... GetDirectResolveAverageSamples ... }` block that ends near 4716) and before the pipeline
loop. First count reason 9 when `link` came in true and that block dropped it (no counter today; needs
`resolve_link_reject_counts_[10]`, V/vulkan_texture_cache.h 476, and a public note function).

Eligible when all hold (count each failing cause in the summary line):
1. `link` still true and `link_mips & 8`.
2. Exactly one dump rectangle covering the whole range: `dump_rectangles_.size() == 1`, `row_first == 0`,
   `rows == dump_rows`, `row_first_start == 0`, `row_last_end == dump_row_length_used` (struct in
   G/render_target_cache.h 491-531; built by `GetResolveCopyRectanglesToDump`, G/render_target_cache.cc 1323).
3. Source `rt_key.msaa_samples == k1X` and `resolve_info.color_edram_info.msaa_samples == k1X` (no read-as-1x, no
   averaging; `vkCmdCopyImage` also needs equal sample counts), not depth, not 64bpp, unscaled.
4. No red/blue swap: `resolve_info.copy_dest_info.copy_dest_swap == 0` (registers.h 880). VERIFIED 20:50Z: the dump shader
   swaps on exactly this bit (edram_dump_shader.cc `kResolveDestInfoSwapShift = 24`, swap 1161-1239; the link
   writes the pixel after the swap, before the endian step).
5. (decided 20:50Z: accept only `VK_FORMAT_R8G8B8A8_UNORM` and `VK_FORMAT_A2B10G10R10_UNORM_PACK32`; the dump
   rounds both exactly; k_8_8_8_8_GAMMA is stored as linear unorm16: reject.)
   `GetColorVulkanFormat(rt_key.GetColorFormat())` (V/vulkan_render_target_cache.h 324) equals the host `VkFormat`
   of every link target (`host_format.format` as chosen in `BeginResolveLink`; images are created with
   `formats[0]`, V/vulkan_texture_cache.cc 1208). Store the format in `ResolveLinkTarget` or add a check helper.
6. No tile-merge source: `TileMergeResolveSource(rt, &x)` (2349) returns the same target with offset 0.
7. `rt_key.GetPitchTiles() == dump_pitch`, `color_edram_info.base_tiles >= rt_key.base_tiles`, no row wrap.
8. Spliced case: VERIFIED SAFE 20:50Z, no `!link_spliced` rule. In a GE-4 splice `SubmitBarriers`
   (vulkan_command_processor.cc 3319) records pending barriers into the splice segment, and the source barrier
   is the same pattern as the dump's render target barrier (vulkan_render_target_cache.cc 4915-4926).

Why bit-exact: `direct_resolve` is bitwise; the EDRAM k_8_8_8_8 dword and the host R8G8B8A8 texel after the load's
endian swap have the same byte order, so without the swap the host texture bits equal the render target bits.
The bench frame compare (12 of 12) is the proof either way.

Source mapping (1x 32bpp tile = 80x16 pixels, xenos.h 452): `delta =
copy_shader_constants.dest_relative.edram_info.base_tiles - rt_key.base_tiles` (the base the shader uses), `dx = delta % pitch`, `dy = delta / pitch`; resolve pixel (x, y) is render target pixel
(`dx*80 + coordinate_info.edram_offset_x_div_8*8 + x`, `dy*16 + edram_offset_y_div_8*8 + y`). Row wrap check:
`dx*80 + off_x + width <= pitch*80`.

Recording (model: `TileMergeCopy`, 2270): `PushDebugMarker`; `TagPassEnd(PassEndReason::kResolve)`; barrier of
the render target image to `TRANSFER_SRC_OPTIMAL` / `TRANSFER_READ` from `current_stage_mask()`,
`current_access_mask()`, `current_layout()` + `SetUsage(...)`; then
`texture_cache.RecordResolveLinkCopy(VK_NULL_HANDLE, 0, 0, link_spliced, rt.image(), src_x, src_y)` which emits
`deferred_command_buffer().CmdCopyImageEmplace(src, TRANSFER_SRC_OPTIMAL, dst, TRANSFER_DST_OPTIMAL, 1)` per
target (V/deferred_command_buffer.h 363) instead of `CmdCopyBufferToImageEmplace`; `*linked_out = true`;
`PopDebugMarker`; `return true`. No dump dispatch, no link scratch, no `shared_memory.Use`.
Render target images need `TRANSFER_SRC`: usage is set at 2733-2738 only under `cvars::xdroid_tile_merge`
(default true); make it unconditional.

Stale memory: the caller (`Resolve`, 1304-1408) still runs `MarkRangeAsResolved` and `EndResolveLink(linked)`,
so a non-linked texture over the same range reloads stale bytes. DIFF frames in the bench mean such a reader
exists. That is what stage A measures: the gain (expected about 28 us per eligible resolve, about 1.0 ms if the
35 mip-chain resolves qualify) is the upper bound of stage B. The sample count of the mip-chain render targets
is unknown: the eligible counter answers it.

Summary line: `EndResolveLink` (V/vulkan_texture_cache.cc 1472-1512) prints `XDROID-TEXLINK frames= links= mips=
kb= unwatched= rejects= by_reason=1/../8` every 600 frames. Add reason 9, image copies, not-eligible by cause,
partial links. No tool parses this line (checked `tools/`).

## Partial-rectangle link: bit 16 (the two halves of 0x1B290000)
- Tiled 2D at 32bpp: macro tile `i = tile_x + tile_y * pitch_tiles` sits at byte `i * 4096`
  (`GetTiledOffset2D`), so one macro tile = one page. The second half has `copy_dest_base` = base + 0x14000.
- `link_candidates_` is `std::unordered_multimap<uint32_t, LinkCandidate>` (G/texture_cache.h 479, member 935).
  Change to ordered `std::multimap` (`<map>` is already included; `equal_range`, `emplace`, `erase` in
  `UpdateLinkCandidates`, G/texture_cache.cc 499, keep working). Add a range lookup: from
  `upper_bound(dest_page)` walk back at most 8 entries, keep those whose level range contains `dest_base`.
- Partial applies to level 0 only, not packed, 32bpp: `index = (dest_base - level_base) >> 12`,
  `tile_x = index % level_pitch_div_32`, `tile_y = index / level_pitch_div_32`, origin in the level =
  (`tile_x*32 + offset_x`, `tile_y*32 + offset_y`).
- In `BeginResolveLink` (V/vulkan_texture_cache.cc 1258-1388): a link "covers the whole level" when the origin
  is (0,0) and width >= level_width and height >= level_height (today's rule, unchanged). Otherwise it is
  partial: needs bit 16, `!texture.base_outdated_lockless()` (else reject 7), origin inside the level, equal
  pitch (reject 3 stays), `start - level_base + length <= align(level extent, 0x1000)`.
- `ResolveLinkTarget` (V/vulkan_texture_cache.h 463) gains `x, y`. Copy extent = `min(width, level_width - x)`,
  `min(height, level_height - y)`, `imageOffset = (x, y)`; the buffer path keeps `bufferRowLength =
  pitch_pixels` (the link scratch is indexed by resolve pixel with the resolve width as pitch).
- `CompleteResolveLink(texture, false)` re-arms the watch on the whole base: same accepted race as reject 7.

## Stage B (only if stage A gains about 0.8 ms or more)
Pending guest writes {range, owner texture + level, resolve constants}. Flush before: a texture load from an
overlapping range, `SharedMemory::RequestRange` (vertex / index / memexport), a later dispatch resolve that
overlaps (drop the pending one when fully covered), a CPU-data upload into pending pages (flush first), readback
/ RESREAD, trace dump, owner texture destruction or eviction. Flush = one dispatch of the existing dump pipeline
(same key, 1x, no `link_scratch`) with the owner texture's level view as source: per-level `VkImageView`, a
source descriptor set with the dump layout from the render target cache's own pool
(`SingleTransientDescriptorLayout` has only `kStorageBuffer` and `kStorageBufferAndStorageImage`), push constants
with the EDRAM offsets zeroed. Verify arm = image copy + immediate flush + targets not marked linked. Then
default on; the switch stays. Never take a lock inside a watchdog region.

## Build and bench (one cycle)
Announce the phone use on the checklist, then one background command, never polled:
`tools/device/phone.sh deploy && tools/device/benchab.sh imgcopy flight.xtr 100 2 "base:"
"link:xdroid_tex_link_mips=5" "unify:xdroid_tex_link_mips=7" "img:xdroid_tex_link_mips=13"
"part:xdroid_tex_link_mips=21" "both:xdroid_tex_link_mips=29"` (about 3.3 min per arm run; arms 5 and 7 also
settle the power question, 2360 against 2465 mW at n=2). Read `docs/runs/<date>/imgcopy-ab.txt`,
`python scratch/ppgaps.py LOG`, the `XDROID-TEXLINK` lines. Then record (oneshot2.md PERF, DONE_LOG, B1,
handoff), commit locally, and revert the temp commit abf844480 once the rework lands.

## Edit pattern
One script per step (`scratch/imgcopy_edit.py`, created with Write, one Bash) using the `edit(path, pairs)`
helper of `scratch/handoff5.py` (exact-match asserts, CRLF kept). Do not re-run the applied scripts in `scratch/`.
