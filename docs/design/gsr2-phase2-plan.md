# GSR2-P2: AC6 world pass at 960x540, GSR2 rebuilds 720p (plan)

Date: 2026-10-22. Follows docs/design/gsr2-plan.md step 9. Option only (cvar `gsr2_world_75`, default off);
the maintainer compares, makes it default only if it looks identical. Explicit one-off exception to "never below 720p".
Executor after the gate: sonnet medium, `superpowers:executing-plans`. Branch xdroid360/gsr2, local commits.

## 0. Gate first (evidence rule 1)

2026-11-07: probe reverted (temp, gate never run; this thread's rule is "never below 720p"). To run the gate,
cherry-pick b854eb849 onto a temp branch first.

Probe built in, not yet run: `gsr2_probe_world_75` (live, temp, revert-planned) in
`VulkanCommandProcessor::UpdateSystemConstantValues` squeezes every world draw (RB_SURFACE_INFO 0x0A010280)
into the top-left 75 % of the viewport: 9/16 of the world pixels shaded and written, image wrong.
- Run: flight trace bench, `benchab` ABBA >= 10 loops, arms `gsr2_probe_world_75=0` vs `=1`, gsr2 off.
- It under-counts the real gain (clears, resolves, GMEM load/store of the 4x target stay full size).
- Proceed only if B saves >= 1.6 ms GPU (Amdahl 1.1x on 17.5 ms). Below that: close P2 in xdroid-perf
  closed experiments with the number, revert the probe.

## 1. Facts

- Upstream already mixes scales per surface: 74db632ab (goldislead, "draw_resolution_scale_threshold")
  puts a `scale_native` class bit in RenderTargetKey (render_target_cache.h:320); caches create, clear,
  transfer and dump per class; native-class resolves write native layout to shared memory
  (`copy_native`, vulkan_render_target_cache.cc:1213-1243) and the direct resolve can read a scaled RT as
  1x (`IsDirectResolveReadAs1xSupported`, .h:320; `DirectResolveRenderTargets`, .cc:4516). Size: 857+/209-
  lines, 23 files, both backends.
- P2 is the same idea with a third class, "reduced" = 3/4 per axis, global scale stays 1x. Every EDRAM
  quantity stays integer at 3/4: tile 80x16 samples -> 60x12, 4x MSAA pixel tile 40x8 -> 30x6, resolve
  8 px granularity -> 6. Because global scale is 1x there is no scaled-resolve buffer: texture cache untouched.
- Only the AC6 world surface (4x MSAA, pitch 1280, plus its depth 0x2D0) is reduced. Post, clouds, HUD
  stay native, as gsr2-plan.md step 9 asks.

## 2. Steps (each: build, flight bench smoke, commit)

1. **Class bit.** `RenderTargetKey::scale_reduced` next to `scale_native`; `IsScaleReducedForPitch()` next
   to `IsScaleNativeForPitch` (render_target_cache.cc:654): true when `gsr2_world_75`, GSR2 title on,
   host RT path, 4x MSAA and pitch 1280. Set it where `scale_native` is set (render_target_cache.cc:734,
   968, 1038, 1586, 1704, 1721). Host RT image size x3/4 (`GetResolutionScaleX/Y` users at .h:384-387 return
   a rational: add `ScaleHostPx(key, guest_px)` = px*3/4 for reduced, px*scale otherwise; use it instead of
   the integer multiply in every key-based size).
2. **Draw.** Per-draw scale (`GetDrawScaleX/Y`, .h:206-210) feeds viewport, scissor, point size and the
   pixel-position divisor. Add `IsDrawScaleReduced()`; viewport/scissor in vulkan_command_processor.cc
   (users at ~426, ~6148, ~7847 and draw_util viewport math) take float 0.75 for reduced draws. Shader
   pixel position: SPIR-V translator modification bit like upstream's native bit (spirv_shader_translator.h)
   so reduced pipelines multiply host px by 4/3.
3. **Clears and transfers.** Clear rects through `ScaleHostPx`. Transfer shader key gets
   `dest_scale_reduced` / `source_scale_reduced` (next to `dest_scale_native`, vulkan_render_target_cache.cc
   ~3807): dest px -> source px = floor(px * src_ratio / dest_ratio), nearest sample.
4. **Resolves.** Reduced RTs never dump to the EDRAM buffer: every resolve whose source is reduced goes
   through the direct resolve with native layout and a new pipeline key bit `source_reduced`
   (source px = floor(dest px * 3/4), bilinear for color off, nearest). Not eligible -> fall back to a
   transfer into a temporary native RT first (log once, count in the glitch recorder).
5. **GSR2 at the world resolve.** When the world color resolve runs and gsr2 is on: GSR2 convert reads
   the reduced 4x color (average samples) and reduced depth (existing `CopyDepth` at 960x540), upscale
   writes a 1280x720 native image, and the direct resolve copy reads that image instead of the RT.
   Jitter in reduced pixels (`2*j/960`, `2*j/540`). Swap: phase-1 GSR2 off when `gsr2_world_75` is on.
6. **A/B.** Live arm `gsr2_world_75` 0/1 on flight bench >= 30 loops (GPU ms, mW) plus the maintainer's
   screenshots side by side; glitch recorder clean. Revert the probe commit.

## 3. Risks

| Risk | Check |
|---|---|
| Surfaces aliasing world EDRAM read it back across classes (MSAA-1X died of this) | step 3 transfers; glitch recorder; first flight with recorder on |
| AC6 resolves world depth (DOF, soft particles) | step 4 covers depth; check resolve log for depth copies from 0x2D0 |
| Not identical image (the maintainer's bar for default) | stays an option; jitter + GSR2 must hide 3/4 |
| Scope: ~400-600 lines, Vulkan only, several builds | gate in section 0 first |
