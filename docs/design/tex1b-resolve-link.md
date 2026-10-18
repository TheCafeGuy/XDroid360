# TEX-1b: resolve-to-texture link (design v3, 2026-10-15)

v1 was red-teamed on three lenses (correctness, Adreno/Turnip, evidence/cost). All three said LOW:
measure first. v2 keeps what survived, drops what didn't, and changes the gate.

## Problem (measured)
- Flight census (ce2): 46-55 texture loads/frame, 27-31 MB/frame. 71-90% are GPU-invalidated
  (post chain: 1280x720 RGBA8 x2, 320x180, 160x90 x5, 208x144, R32F 64/16/4). The rest are CPU reloads
  of identical bytes, which TEX-1a (97db686eb) already skips.
- Per render-to-texture on Adreno today (scratch blit off on Qualcomm, vulkan_texture_cache.cc:3536):
  resolve dispatch (host RT or EDRAM -> shared memory = guest RAM, zero-copy) -> watch fires ->
  at the sampling draw: render pass ends, load dispatch (detile + endian) into a scratch buffer,
  vkCmdCopyBufferToImage into the texture. 2 dispatches + 1 copy + barriers + a pass break per texture.
- Turnip lesson: passes and barriers cost more than bytes (scratch blit: 7.5 vs 4.4 ms/frame).

## What the red team changed
- Mode A (vkCmdCopyImage RT -> texture) dropped. RTs lack TRANSFER_SRC, adding it risks UBWC (UBWC-1),
  non-direct resolves read the EDRAM buffer (not an RT image), and several RT host formats are not the
  guest bits (gamma, 7e3, 16_16_16_16). RT aliasing (sample the RT directly) parked: EDRAM is reused by
  the next pass, so the RT is usually gone by sampling time.
- Gate changed from "% of loads linkable" to GPU ms and pass breaks (loads range 60 KB to 3.6 MB).
- Upstream-tree surgery (resolve shaders, watch ordering) = architecture change: STOP item, needs
  the maintainer's approval before Phase 1.

## Phase 0 (shipped 1495fcd6f, diagnostics only)
xdroid_tex_census now records every resolve and classifies every GPU-invalidated load: linkable
(direct / EDRAM fast resolve) or the first failing condition (no_record, not_fast, offset, pitch,
height, format, endian, coverage, mips, shape), with counts and KB/frame per class, per scene.
Still to add if Phase 0 says go: GPU timestamps around linkable load dispatch+copy (ms/frame), and
whether each load ended a pass.
Kill rule: linkable < 1.0 ms/frame of GPU in both flight and cutscene, or < 5 pass breaks removed/frame.
Runs: 3 flights + the s1cine cutscene, same build, cool phone, battery > 30%, unplugged.

## Phase 1 design (Mode B, batched)
1. Exact-match link at resolve time. Link only if ALL hold (else today's path):
   a. resolve dest offset 0 and extent start == texture base; b. aligned pitch and aligned height equal;
   c. extent covers the texture's whole guest base size; d. no packed mips, mip_max_level 0, no mip page;
   e. 2D, tiled, not scaled, no array; f. copy dest format == texture format and dest endian == texture
   endian; g. fast copy shader (bit move: sample select, half-pixel fill x and y, R/B swap); h. texture
   host format is a pure copy (R8G8B8A8 or R32_SFLOAT load, no host conversion).
2. The fast resolve shader variant also writes its pixel values after swap/fill and BEFORE the endian
   swap and tiling into a scratch buffer in the texture's host layout (exactly what the load produces).
   Both resolve shaders get it: direct (host RT) and EDRAM.
3. Batch: pending copies queue up; before the next BeginRenderPass (or any read of the texture) emit
   ONE compute-write -> transfer-read barrier, N vkCmdCopyBufferToImage, ONE transfer-write -> shader-read
   barrier (plus write-after-read barrier and layout transition for textures sampled earlier in the same
   command buffer). Copies stay in resolve order. Render-pass begins per frame must not rise.
4. In the same global critical region as MarkRangeAsResolved: re-arm the texture's watch and clear its
   outdated flag (MakeRangeValid already ran, so IsRangeValid passes). No new lock; not reachable from a
   watchdog region (hard rule 8). MarkAsUsed pins the texture for the submission.
5. Anything else that writes the range later (CPU, memexport, another resolve) fires the watch as today.

## Verification
- Verify mode cvar: run the link AND the normal load into a shadow image, compare bit-exact on the GPU,
  log XDROID-TEXLINK mismatch with the key. CPU-write fuzz cvar between resolve and draw.
- Ship gate: 0 mismatches over 3 flights + menus + cutscenes; glitch catcher clean; live A/B
  xdroid_tex_link off/on: texload pass ends, gpu_ms, render-pass begins, mW. Proceed only if the texload
  gap falls >= 40%.

## Expected
Bound: texload work 2.75 ms/frame (9% of 30 ms, ~1.10x, on the reject line; the saving is a share of
it). The case for it is the cutscenes (gpu_busy 34-38 ms/frame) and energy at the cap, so Phase 0 decides.

## Later (separate approval each)
- Skip the guest write when nothing reads it (needs a read-fault probe proving the CPU never reads).
- Batch the remaining (CPU-side) loads before a pass.

## Round 2 red team -> v3 changes
- GE-4 already proved pass breaks are nearly free on Turnip: xdroid_pass_splice=3 cut 232 -> 187 passes/frame
  with no GPU time change (DONE_LOG 2026-10-14, r3perf). So TEX-1b must remove bytes and dispatches, not
  pass breaks. Eager-load (run the existing load at resolve time) only moves work: rejected.
- Ordering hole fixed: with the resolve splice on, the link's copy goes into the same splice segment, and
  the texture's outdated flag is cleared at flush time, not at resolve time. Textures sampled earlier in
  the open pass (sample_draw_serial) get a write-after-read barrier or no link.
- Only link textures that were sampled in the previous frame (no work for resolves nobody reads).

## v3 build options, chosen by Phase 0 data (per linked texture)
| Option | Work per render-to-texture | Notes |
|---|---|---|
| Today | 3 reads + 3 writes, 2 dispatches + 1 copy | |
| Mode B: resolve also writes scratch, batched CopyBufferToImage | 2R + 3W, 1 dispatch + 1 copy | texture keeps UBWC |
| Write-through: resolve imageStores the texture too | 1R + 2W, 1 dispatch | needs STORAGE usage on linked textures; check UBWC loss on A750 with a micro A/B |
| Phase 3: + skip the guest write | 1R + 1W | only with a read-fault probe proving the CPU never reads those pages |

Bound: <= 2.2-2.75 ms/frame of GPU (the GPU-invalidated share of the texload work); realistic 0.7-1.5 ms.
Kill if Phase 0 shows < 1.0 ms/frame linkable in both flight and cutscene.
