# GSR2-1: Snapdragon GSR 2 temporal upscale, default on (plan)

Date: 2026-10-21. Research: docs/design/gsr2-research.md. Executor: sonnet medium, `superpowers:executing-plans`.
the maintainer's answers (thread "GSR2"): wants both image quality and, later, lower internal resolution; GPU may go
over 18 ms for now; GSR2 sharpens fully, the phone keeps only its frame interpolator
(`present_oem_postprocess=interpolate`); camera motion comes from AC6. Pending (defaults used until he answers):
output 1080p, phase 2 at 75 % (960x540), phases in order.

## 1. Facts this plan rests on (verified on docs/runs/2026-10-15/traces/4E4D07D1_stream.xtr, frames 0-12)

Offline parser: `tools/device/xtr_consts.py` (step 1). PM4 register replay incl. LOAD_ALU_CONSTANT from the
recorded memory reads.

| Item | Value |
|---|---|
| World pass | ~1266 draws/frame, `RB_SURFACE_INFO=0x0A010280` (2x MSAA), viewport scale 640/-360 offset 640/360 (1280x720), scissor 0,0-1280,720, depth `RB_DEPTH_INFO=0x000102D0` (D24FS8, EDRAM base 0x2D0). No predicated tiling |
| After it | 4x MSAA 640x360 pass (~272 draws, effects/clouds), 1x 1280 passes (compositor, then 2D/HUD with the ortho matrix c40-c43) |
| VS constants (world draws, shared by >50 % of them) | c64-c67 projection: 1.3492, 2.3986, z 4.1668e-05 / 1 = reverse-Z infinite. c104.x = 0.74118 = tan(hfov/2) (GSR2 `cameraFovAngleHor`). c112-c115 view. **c116-c119 view-proj of this frame, c120-c123 = previous frame's c116-c119** (checked frames 0-7: c123[f] == c119[f-1]). Registers are columns: clip = x*c116 + y*c117 + z*c118 + c119 |
| Camera | c44-c47 camera-to-world (position moves 5-75 units/frame in flight) |

So AC6 hands us both matrices: `clipToPrevClip = VP_prev * inverse(VP_cur)` with VP_cur = c116-c119,
VP_prev = c120-c123, no history of our own. Which of the two the world VS really uses is inferred (c116 = the
newer one); step 6 checks it with arm `gsr2_vp_swap`.

## 2. Design (one stage, inside the CP swap, like FXAA)

`VulkanCommandProcessor::IssueSwap` already runs gamma (compute) into an intermediate
(`fxaa_source_image_`) and FXAA into the presenter's guest output. GSR2 takes the FXAA slot:
gamma -> 720p intermediate (reuse the FXAA source image) -> GSR2 convert (CS) -> GSR2 upscale (CS) ->
guest output at 1920x1080. The presenter sees a 1080p guest output and only copies it (present scale 75 %
gives a 2376x1080 swapchain, 1:1 in height; display hardware stretches to the panel).

Inputs, all captured in the CP:
- **Depth**: at the first draw that leaves the world surface (surface != 0x0A010280 after >=100 world draws),
  copy sample 0 of the host depth RT at EDRAM base 0x2D0 into an R32F 1280x720 image (one small compute
  pass; reuse how `VulkanRenderTargetCache::DumpRenderTargets` addresses host RTs).
- **Matrices + FOV**: at that same draw, read c116-c123 and c104 from the register file
  (`XE_GPU_REG_SHADER_CONSTANT_000_X + 4*i`).
- **Jitter**: Halton(2,3) 8-phase sub-pixel offset added to `system_constants_.ndc_offset` (vulkan_command_processor.cc
  ~8382) only for world-surface draws. HUD and compositor never jitter.
- **HUD mask**: pixels drawn after the world pass never get history. GSR2 2-pass has an alpha-mask channel
  (`MotionDepthClipAlphaBuffer.w`, unused in 2-pass); write 1 where the final frame differs from a copy of
  the world resolve, and in upscale use the current sample only there.
- `reset` = 1 on: first frame, guest output size change, world pass missing this frame (menus, hangar,
  cutscene cuts: camera jump > 500 units). Menus/front-end (no world pass) fall back to GSR1 path.
- Reverse-Z: flip `min` to `max` for nearest depth and the depthclip test (README "Pass NDC Considerations").

Title gate: AC6 only (4E4D07D1); the constant rows are AC6 facts. Placement: shaders in
`src/xenia/gpu/shaders/sgsr2_{convert,upscale}.cs.slang` (ported from `sgsr/v2/include/glsl_2_pass_cs`,
header + BSD-3 kept, source commit `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e`); AC6 capture logic in
`xdroid-core/src/main/cpp/xdroid_ac6_gsr2.cc` through a draw hook; CP edits limited to the hook call, the
jitter add, the depth copy and the swap dispatch.

Variant: 2-pass **CS** first (fits the swap's compute-only path, fewer lines). Qualcomm's table says 2-pass FS is
~2x cheaper on Adreno (0.9 vs 1.8 ms at 1260x2800, max clock). If CS costs > 1.2 ms/frame on the bench, port the
FS variant as an arm (step 7).

## 3. Switches (all live, defaults set in xdroid_defaults.cc only at step 8)

| cvar | Values | Default after step 8 |
|---|---|---|
| `gsr2` | bool | true if gate passes |
| `gsr2_output_height` | 720 / 1080 / 1440 | 1080 |
| `gsr2_jitter` | bool | true |
| `gsr2_vp_swap` | bool (use c120 as current) | from step 6 |
| `gsr2_hud_mask` | bool | true |
| `present_resolution_scale_percent` | | 75 when gsr2 on |
| `present_oem_postprocess` | | interpolate |

## 4. Steps

1. **Tool**: move the scratch parser to `tools/device/xtr_consts.py` (prints per-frame world pass size and
   rows c104, c116-c123; `--frames N`). Commit.
2. **License**: `docs/licenses/notices.tsv` row "Snapdragon Game Super Resolution 2 ... BSD-3-Clause ...
   snapdragon-gsr-LICENSE.txt". Commit.
3. **Shaders**: port convert + upscale (2-pass CS) to slang next to `fxaa.cs.slang`; inputs as above; reverse-Z
   edits; alpha-mask use in upscale. Compile with the pinned slangc (see gsr-oem-coexistence.md build note:
   re-run CMake configure, delete the Gradle `.cxx` cache). Commit.
4. **Capture**: draw hook + `xdroid_ac6_gsr2.cc` (world-pass detection, matrices, FOV, reset rules, Halton
   index); depth copy pass; jitter add. Log once per 300 frames: `XDROID-GSR2 world=1 draws=N depth=1
   cam_jump=.. reset=..`. Commit.
5. **Swap stage**: images (MotionDepthClip RGBA16F 1280x720, ColorLuma R32UI 1280x720, history x2 RGBA8 at
   output size, swapped per guest frame), UBO, two dispatches, guest output size = output size when active.
   Build, install, run `phone.sh` bench smoke (gpubench replays go through IssueSwap, so GSR2 runs there):
   no Vulkan validation errors, `XDROID-GSR2` line present. Commit.
6. **Motion check**: liveab on the replay, arms `gsr2_vp_swap=0/1`; pick the one with lower history rejection
   (log the mean depthclip/reject ratio per window); the maintainer screenshots of a turn (ghosting check). Commit default.
7. **Cost**: `benchab` >= 30 loops ABBA, arms `gsr2=0` (today: present 50 %) vs `gsr2=1` (present 75 %). Report
   ms/frame median/p95, `Present stats` gpu_us/paint, energy (xdmeasure) on a flight. If > 1.2 ms, add the FS
   variant arm and repeat.
8. **Default**: if no glitches in the maintainer's flight (glitch recorder clean, HUD sharp, no ghost trails) and no
   crash/DEVICE_LOST, set defaults (section 3) in `xdroid_defaults.cc` (bump defaults version), DONE_LOG row,
   UI entry in SettingsCatalog.kt ("Snapdragon GSR 2").
9. **Phase 2 (separate plan, opus high)**: render only the world pass at 75 % and let GSR2 rebuild 720p at
   the world resolve, before clouds/HUD (HUD stays native). Needs a host RT for the world surface smaller than
   its EDRAM footprint: architecture change, approved in principle by the maintainer (answer 1b); spike first.

## 5. Risks

| Risk | Check |
|---|---|
| Wrong VP order: double ghosting | step 6 arm |
| Gpubench frame hashes change by design (jitter, temporal) | quality = the maintainer screenshots + glitch recorder, hashes compared only with `gsr2=0` |
| Depth copy reads a stale RT (EDRAM reused by the 4x pass) | copy at the first non-world draw, before any other surface binds |
| Swapchain recreate on toggling present scale | change before flying (one-frame gap) |
| Other titles | gated to AC6; others keep GSR1/bilinear |
