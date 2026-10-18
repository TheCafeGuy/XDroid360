# GPU time cuts (Turnip / Adreno 750) - design, 2026-10-13

Baseline (mission 6, aud-m6.log f:4259-4379, forensics off): GPU busy 25.0-25.9 ms/frame,
180-190 passes/frame, ends/frame rt 40, resolve 66, barrier 29-33, upload 21, texload 20,
submit 4. Target: <= 14 ms (60 FPS), <= 25 ms (40 FPS).

All ms figures below are **inferred** (bandwidth model, not measured): a 640x2048 pass with
8888 color + D24S8 moves ~21 MB in GMEM mode (load + store), ~0.3-0.4 ms at 50-70 GB/s.
Turnip's autotune runs small passes in sysmem mode, where load/store costs nothing, so the
real split is unknown until XR-3 runs. **XR-3 first; it turns every row into a measurement.**

| Item | Exact change | Est. saving | Glitch risk | Switch (live) |
|---|---|---|---|---|
| XR-3 per-pass profiler (**implemented**) | Bottom-of-pipe timestamps before each pass begin and after its end, one frame in N; logs per kind (guest/transfer/clear), per end reason, between-pass time, top-K passes (WxH, draws, reason, ms) | 0 (diagnostic) | None; small GPU waits on the profiled frame only | `xdroid_pass_profiler` N, `xdroid_pass_profiler_top` |
| GE-2 clear-only passes (**implemented**) | Resolve clear into an RT with no transfers in that call: pass area = clear rect (origin kept, far edges up to 64 px) instead of 640x2048; mode 2 clears via LOAD_OP_CLEAR with the exact rect (no load at all) | 0.5-4 ms (10-25 clear passes/frame x 0.05-0.25 ms) | Low: spec-exact (outside the area the image is untouched; the pass is never reused). Mode 2 uses Turnip's unaligned-edge store path | `xdroid_clear_pass_mode` 0/1/2 |
| GE-1 tight guest area (exists) | `vulkan_tight_render_area`: guest pass area = union of draw scissors (~720 of 2048 rows) | 3-8 ms (largest single item if guest passes run in GMEM) | Known: hangar hexagon "barcode". Every in-pass write is a scissored draw, so the suspect is Turnip LRZ state across passes with different areas (8x8 LRZ blocks = stripes) | live already; falsify with one hangar run `TU_DEBUG=nolrz` + tight area on |
| UBWC-1 (exists) | `vulkan_render_target_format_list`: format list on mutable color RTs keeps UBWC | 1-4 ms (20-40% of color RT bandwidth) | Low; image creation only | boot cvar, one run per arm |
| GE-4 pass splice (**implemented**) | New deferred command `kExecuteSegment`: a placeholder written right before every guest pass begin; texture loads and shared-memory uploads recorded while the pass is open go into that segment (runs before the pass) instead of ending it. Refused (pass ends as before) when: a draw already recorded in the pass sampled the image (per-texture draw serial) or read one of the pages (per-page draw serial, marked in `SharedMemory::RequestRange` during the draw); a memexport draw is in the pass; a scaled-resolve texture; any other pending barrier (RT/attachment images). Barriers left by spliced work (texture to shader-read, shared memory to read) join the segment at the next draw when the pass is reused | 4-10 ms (texload 20 + upload 21 ends/frame, plus part of "barrier") | Medium. By construction an earlier draw never sees new data. HUD-1: guest-side race, fixed by `ac6_hud_slot_join`; GE-4 only reorders GPU work and refuses uploads of pages HUD draws already read in the pass. Test GE-4 with `ac6_hud_slot_join` on | `xdroid_pass_splice` bit 1 = texture loads, bit 2 = uploads |
| Resolve splice (**implemented**) | A resolve copy whose source tiles are owned by no render target bound in the open pass, into pages no recorded draw of the pass read, is recorded into the same segment. Its clear (if any) still ends the pass. Not with resolution scaling or debug markers | 2-6 ms if half the 66 resolves/frame qualify | Medium-high (EDRAM ownership is checked from the current owners) | `xdroid_pass_splice` bit 4 |
| GE-3b barrier relabel | `DumpRenderTargets` (rtc ~4633) calls `SubmitBarriers(true)` untagged, so part of "barrier" is really resolve | 0 (diagnostic) | None | n/a |
| GE-5 depth DONT_CARE | With dynamic rendering, storeOp sits in the recorded `VkRenderingAttachmentInfo` (patchable like `PatchRenderArea`). Remember the last pass that stored depth RT X; if a later full-area clear of X comes with no resolve/transfer reading X in between, patch that store to DONT_CARE | 0.5-2 ms | Medium: a missed reader = garbage depth (shadows, depth-of-field) | `xdroid_depth_store_elide` 0/1 |
| Transfer area | Ownership-transfer passes: area = union of transfer rects | <0.5 ms (1/frame) | Low | fold into GE-2 later |

## Rotation plan (one flight, `phone.sh liveab`)

1. `xdroid_pass_profiler=30` on the whole flight (one profile per second): gives ms per kind and
   per end reason, which ranks GE-1/GE-2/GE-4 by measurement.
2. Arms: `xdroid_clear_pass_mode` 0 / 1 / 2 (control = 0). Kill an arm on any new mark, or if
   `clear` ms in the PassProf line does not drop vs arm 0.
3. Separate hangar run: `vulkan_tight_render_area` on, with and without `TU_DEBUG=nolrz`; watch the
   stat hexagon. Barcode gone with nolrz = LRZ is the cause and GE-1 needs an LRZ-safe variant.
4. Boot pair: `vulkan_render_target_format_list` false / true.
5. GE-4 arms: `xdroid_pass_splice` 0 / 1 / 3 / 7 (risky last). The frame stats add
   `Spliced/frame (GE-4): texload upload resolve barrier`; pass ends/frame must drop by about that
   much. Kill an arm on any new mark (textures one frame stale, flickering geometry, HUD).

What to look at (tester): menus' fades (clears), HUD, cockpit/hangar hexagon, shadows, clouds.

## HUD-1

GE-2 and XR-3 do not touch uploads, guest timing or draw order: they only change the area/load
op of clear-only passes and add timestamps. GE-4 only reorders GPU work (no guest
timing) and never lets an earlier draw see new data; still, run its arms with `ac6_hud_slot_join`
on so a HUD mark can't be blamed on the wrong change.
