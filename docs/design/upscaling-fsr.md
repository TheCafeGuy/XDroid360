# UPS: upscaling / FSR on Android (analysis + UPS-1)

Date: 2026-10-14. Code/analysis only, nothing run on the phone yet.

## 1. What already exists

| Piece | Where | State on device |
|---|---|---|
| FSR 1.0 (EASU + RCAS) and CAS on the final output | `src/xenia/ui/presenter.cc` (effect flow ~L880-990), shaders `src/xenia/ui/shaders/guest_output_ffx_fsr_*` | Available via `postprocess_scaling_and_sharpening = fsr` / `cas`; device runs the default (bilinear) |
| Internal resolution scale | `draw_resolution_scale_x/y` (`src/xenia/gpu/texture_cache.cc:30`) | **Integer 1-7 only**, needs sparse binding above 1x. Device runs 1x (`docs/runs/2026-10-13/hud1-c2.log:104`) |
| Guest output | `internal_display_resolution` = 1280x720 (`hud1-c2.log:288`) | |
| Swapchain | 1440x3168, pre-rotated (`hud1-c2.log:491`) | Full panel resolution |
| GPU time | `Frame stats ... gpu_busy_us/frame` (`vulkan_command_processor.cc:2176`) | 25.5 ms clean. Covers the emulator submissions only, **not** the presenter |

## 2. Why "render lower + FSR up" cannot cut the 25.5 ms today

- There is no sub-1x path. Xenia's render targets are EDRAM-addressed in guest pixels; scale only multiplies
  (tile math in `render_target_cache.cc`, scaled resolve buffers in `texture_cache.cc`, half-pixel /
  texel offsets in the shader translator). A fractional scale would need: fractional host RTs, a
  downscaled resolve store that texture fetches sample with scaled coordinates, and CPU readbacks of
  resolved data upscaled back to guest size. That is an architecture change (plan rule B0.8, needs
  the maintainer's OK) and weeks of work with high glitch risk.
- The 3x DEVICE_LOST is on the >1x path (sparse binding on Turnip). Keep scale at 1x; nothing here touches it.
- **The real "fewer pixels" lever is MSAA, not resolution.** AC6 draws into 4xMSAA render targets
  (`hud1-c2.log:2545`: "RT ... 4xMSAA, k_8_8_8_8"). On Adreno, 4x samples shrink GMEM bins 4x and
  multiply every EDRAM load/store (we have many passes per frame, GE-0). A "MSAA-1X" arm (host draws the
  4x targets with 1 sample, resolve becomes a copy) is the candidate that could move GPU ms by a large
  amount. Unknown size until the per-pass profiler (XR-3) or a plain A/B. Risk: games that reinterpret
  EDRAM between sample counts. Proposed as the next GPU item (see section 5).

## 3. What UPS-1 adds (this branch, off by default)

`present_resolution_scale_percent` (int, default 100 = off, 25-100, **live**, Android only):
the swapchain is created at that percent of the window size; Android's swapchain always uses
`SCALE_TO_WINDOW`, so SurfaceFlinger / the display hardware stretches it to the panel for free.
The guest output stretch, FSR/CAS, the UI overlay and the pre-rotation copy all run on the smaller image.

- 50% on this phone: 1584x720 swapchain, 1:1 in height with the 720p game (4x fewer pixels than now,
  and the pre-rotation path writes the frame twice, so ~9 MP -> ~2.3 MP per frame).
- Makes `fsr` affordable at present if the maintainer wants the sharper look: at 50% EASU only stretches width.
- Live change: added to `GetSwapchainCreationCvars()`, so a change recreates the swapchain (one frame).
- Guard: if the window reports the previous scaled buffer size, it is not scaled again (log line
  `Presenter: present resolution scale N%: window WxH -> WxH` shows every change).
- Logging: new `Present stats: gpu_us/paint=... paints=... swapchain=WxH native=WxH scale=N%` every
  300 paints (timestamp queries around the paint command buffer). Read it next to `Frame stats`.

Expected gain (inferred, measure): present GPU work is likely ~0.5-2 ms of the shared GPU per frame at
full res; 50% should remove most of it, plus bandwidth/energy. It does **not** close the 25.5 -> 14 ms gap.

Files: `src/xenia/ui/presenter.{h,cc}` (cvar, `ApplyPresentResolutionScale`),
`src/xenia/ui/vulkan/vulkan_presenter.{h,cc}` (live recreate, query pool, `CollectPaintGpuTime`).
Syntax-checked with clang `-fsyntax-only` in an Android configuration (stubbed NDK headers); not built
with the NDK.

## 4. Dynamic mode (design, not implemented)

Belongs to the adaptive 60/40/30 tier controller, not a separate loop. Knob order, cheapest visual cost first:
1. FPS target tier (60 -> 40 -> 30), driven by `gpu_busy_us/frame` + present `gpu_us/paint`.
2. Present scale 100 -> 50 (one-frame hitch per change; change at most every ~10 s, hysteresis 15%).
3. MSAA-1X if it lands and passes the glitch gate.
Thermal input: Android thermal status/headroom lives in Java (`PowerManager`), which means a JNI addition
(Matrix trigger, needs the maintainer's OK). Without it, the controller can use GPU ms trend only.

## 5. Next steps

| ID | What | Who |
|---|---|---|
| UPS-1 verify | Arms 60 s each, same flight route: `100/bilinear`, `50/bilinear`, `50/fsr`. Read `Present stats`, `Frame stats` (fps, gpu_busy), glitch log. Check HUD text sharpness, touch overlay alignment, and that the scale log line appears once per change (no compounding) | PC |
| MSAA-1X | Implemented off by default (`gpu_force_msaa_1x`), see docs/design/msaa-1x.md | PC runs |
| Sub-1x | Only if MSAA-1X is not enough; needs the maintainer's approval (architecture change) | The maintainer decides |
