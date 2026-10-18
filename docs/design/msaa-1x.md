# MSAA-1X: draw the game's MSAA targets with one sample

Date: 2026-10-14. Code/analysis only, not run on the phone yet. Follows docs/design/upscaling-fsr.md section 2.

## 1. Evidence and expected gain

- AC6 draws into 4xMSAA targets: `RT @ 0t, <6t>, 4xMSAA, k_8_8_8_8` (14 hits) and `<16t>, 4xMSAA` (4 hits) in
  `docs/runs/2026-10-1*/*.log` ("Awaited real pipeline" lines, so this only shows the pipelines that
  stalled). **How much of the frame is 4x is not known yet**; the switch adds the line that answers it.
- Mission GPU: ~30 ms at p90 with ~200 passes/frame (`docs/runs/2026-10-13/hud1-m.log`, Frame stats);
  pass ends per frame: resolve 66, rt 41, barrier 37, texload 23, upload 22.
- On Adreno/Turnip each pass loads and stores its attachments through GMEM. A 4x target holds 4x the
  bytes, needs 4x the GMEM, so more bins and 4x the load/store and resolve traffic. 1280x720 4x color+depth
  is ~29 MB per full load or store, roughly 1 ms of bandwidth for a load+store pair (inferred, ~60 GB/s).
- Expected (inferred, unmeasured): close to nothing if only small effect targets are 4x; a few ms (3-8)
  if the main scene is 4x and is reloaded across many passes. The first device run decides.

## 2. Change (off by default)

`gpu_force_msaa_1x` (bool, GPU section, live). `VulkanCommandProcessor::ApplyForceMsaa1x()` clears the
sample count in `RB_SURFACE_INFO` at the top of `IssueDraw` (after the copy-mode branch) and `IssueCopy`.
Every reader (render target cache, resolve info in draw_util, shader modifications, system constant
flags, draw extent estimator) runs inside those two calls, so writing and reading EDRAM always agree on 1x.
Footprints shrink (1x WxH is a prefix of the 4x region at the same base), so no new EDRAM overlaps appear.
Live toggling reuses the existing ownership transfers between sample counts
(`render_target_cache.cc:1839`).

New log line, every stats window, switch on or off:
`MSAA draws/frame: 1x=.. 2x=.. 4x=.. forced_1x=..` (draws by the game's sample count; forced counts draws
and resolves rewritten).

Files: `src/xenia/gpu/vulkan/vulkan_command_processor.{h,cc}` (~60 lines). Syntax-checked with clang
`-fsyntax-only` in an Android configuration (stubbed NDK and shader headers); not built with the NDK.

## 3. Risks and how to see them

| Risk | Why | Check |
|---|---|---|
| Jagged edges | That is the point: no edge AA (main visual cost) | Screenshots A/B at the same spot |
| Alpha-to-coverage | Clouds/foliage that use alpha-to-mask become a 1-sample dither (`spirv_shader_translator_rb.cc:3882`), visible as screen-door edges | Clouds and smoke close up, HUD-1 glitch detector |
| Reinterpretation | A game that reads 4x EDRAM as a 1x surface twice as wide/tall (custom downsample, depth tricks) sees a different layout now | Look for new glitches in post effects (bloom, heat haze, lens flare), glitch log |
| Resolves | Resolve becomes a copy instead of a 4-sample average; sample-select resolves (depth) read the only sample, which is fine | Depth-based effects (soft particles, DOF) |
| Depth precision / Z-fighting | Unchanged depth format; one sample per pixel | Distant terrain flicker |
| Half-pixel offset | Same as the 1x path the game already uses for 1x targets | Thin lines, HUD text |
| Live toggle hitch | Ownership transfers on the first frame after a change | One-frame hitch is expected, nothing after |

Not touched: draw_resolution_scale (stays 1x; the 3x DEVICE_LOST path is unrelated), sub-1x (on hold
for the maintainer).

## 4. Device test (PC)

1. Switch off, one flight 60 s: read `MSAA draws/frame` and `Frame stats` (gpu_busy_us/frame). If 4x draws
   are a small share, stop here and log MSAA-1X as no lever.
2. Arms 60 s each, same route, cool phone: `off` / `on` / `off`. Read gpu_busy_us/frame p50/p90, fps,
   glitch log, screenshots of clouds, HUD and the hangar.
3. Keep only if gpu_busy drops beyond 2x noise and no new glitch class appears; then decide with the maintainer
   whether the edge quality trade is acceptable (could be a tier of the adaptive 60/40/30 controller).
