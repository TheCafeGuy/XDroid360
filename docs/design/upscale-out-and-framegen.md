# Output upscaling (720p -> 1080p/1440p) and frame interpolation to 60

Date: 2026-10-14. The maintainer's direction: no rendering below 720p; upscale the native 720p output to
1080p/1440p, and interpolate to 60 when native 60 (AC6 timing hooks, design/ac6-60fps.md) is not worth it.
Code/analysis only. Builds on docs/design/upscaling-fsr.md (UPS-1).

## 1. 720p -> 1080p/1440p: FSR on the output vs 2x internal scale

| Option | GPU cost on this phone | Risk | Verdict |
|---|---|---|---|
| **A. Native 1x + FSR 1 on present, swapchain capped to 1080** (`present_max_height=1080`, `postprocess_scaling_and_sharpening=fsr`) | EASU + RCAS on ~2.6 MP (2376x1080): estimated ~0.5-1.5 ms (inferred; `Present stats` measures it). The display stretches 1080 -> 1440 for free | Low: present path only, already in Xenia | **Default candidate** |
| A'. Same at full 1440 (`present_max_height=0`, `fsr`) | EASU from 720 to 1440 = 2x, ~4.6 MP, plus the pre-rotation copy of 4.6 MP: estimated ~1.5-3 ms (inferred) | Low | Only if 1080 looks soft |
| B. `draw_resolution_scale` 2x (1440p internal) | Renders 4x the pixels: the pixel-bound part of the current 25.5 ms GPU grows up to 4x, so ~50-80 ms per frame (inferred, ~15 FPS); needs sparse binding; 3x already caused DEVICE_LOST | High | Rejected for this phone |
| C. Snapdragon GSR 1 (one-pass spatial upscaler tuned for Adreno, github.com/SnapdragonGameStudios/snapdragon-gsr) | Single pass instead of EASU + RCAS, so cheaper than A (inferred) | Low, but a new vendored shader | Proposed: new dependency, needs the maintainer's OK (license to confirm, believed BSD-3) |

New in this branch: `present_max_height` (int, 0 = off, live): caps the shorter side of the swapchain after
`present_resolution_scale_percent`, so "1080" means the same on any panel (no device detection by name).
Logged in the `Presenter: present resolution scale` and `Present stats` lines.

Test (PC, 60 s each, same flight): `0/bilinear` (today), `1080/fsr`, `0/fsr`. Read `Present stats`
gpu_us/paint and `Frame stats` gpu_busy; take screenshots of the HUD text and terrain.

## 2. Frame interpolation to 60

Budget at 30 real FPS: 33.3 ms of GPU per real frame. Today 25.5 ms (guest) + present. A generated frame
must cost <= ~5 ms to fit cool, and nothing is left hot (inferred from ac6-60fps.md section 5).

| Option | Needs | Fits? |
|---|---|---|
| FSR 3 frame generation (MIT) | Game motion vectors + depth, desktop-class compute (optical flow + several passes) | No: Xenia has no motion vectors, and the pass count is far above a 5 ms Adreno budget (inferred) |
| Qualcomm Adreno Frame Motion Engine (AFME) | Qualcomm's proprietary driver (sample in SnapdragonStudios adreno GLES framework) | No on Turnip (inferred; verify if a vendor-driver mode ever ships) |
| OEM display interpolation (MEMC chip) | Phone whitelist for game packages | Out of our control; check if OnePlus 12 offers it per app [verify] |
| **Generic optical-flow interpolation** (block matching on a 1/4-res luma + warp) | Two real frames: +1 real frame of latency (~33 ms) | Maybe 2-4 ms per generated frame (inferred). HUD and missile trails smear |
| **Camera reprojection with depth (AC6-specific, VR "spacewarp" style)** | The last real frame, its depth (EDRAM) and the camera matrices from AC6's vertex constants | Best fit: one full-screen pass (~1-2 ms, inferred), no added latency (extrapolates), sky and terrain (most of the screen in flight) are exact; other aircraft and missiles ghost slightly |

HUD rule for either generated path: interpolate the world image only and draw the HUD from the latest real
frame on top. AC6 already gives the split: the CP stamps the world compositor draw (pixel shader
`17e5e4ac3e713245`, ac6-60fps.md section 3), so the image before the HUD can be copied there.

Pacing: the presenter must show real and generated frames at an even 16.7 ms. Today it paints when the
guest output changes; a generated frame needs a second paint per guest frame, timed off the display vsync.

## 3. Recommendation and order

1. Now (cheap, done): `present_max_height` + FSR on the output. Measure 1080/fsr against today.
2. Native 60 stays first where the GPU allows it (ac6-60fps.md P60-0 headroom probe decides).
3. If native 60 is out of reach in flight: prototype camera reprojection (FG-1) behind a switch, world-only
   with the real HUD on top, extrapolated so it adds no latency. Needs: the AC6 camera constant location
   (PC disassembly or a constant dump), a depth copy at the world compositor stamp, one compute pass in the
   presenter, and a second paint per guest frame. Multi-file presenter + CP work, so it gets its own
   Proposal Card and red team before code.
4. GSR 1 instead of FSR 1 on the output: ask the maintainer (new vendored dependency).
