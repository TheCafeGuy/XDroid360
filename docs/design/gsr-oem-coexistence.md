# GSR-1 and OEM-1: Snapdragon GSR, living with the phone's own game features

Date: 2026-10-14. Code only, not built with the NDK, not run on the phone. Follows
docs/design/upscale-out-and-framegen.md.

## 1. License (confirmed)

SnapdragonGameStudios/snapdragon-gsr at `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e`: `LICENSE` is
BSD-3-Clause (Qualcomm Innovation Center), and the shader header carries `SPDX-License-Identifier: BSD-3-Clause`.
Ported GSR 1 (spatial, one pass) from `sgsr/v1/include/glsl/sgsr1_shader_mobile.frag`, RGBA mode. Copyright
and license kept in the shader header; `docs/licenses/notices.tsv` + `docs/licenses/snapdragon-gsr-LICENSE.txt`
added so the in-app notices list it. GSR 2 (temporal) needs motion vectors Xenia doesn't have: not ported.

## 2. What was added (all off by default, all live)

| Switch | Effect |
|---|---|
| `present_snapdragon_gsr` (bool) | When the output is bigger than the game image, one GSR pass does the whole upscale instead of FSR/CAS/bilinear. Vulkan only (`IsSnapdragonGsrSupported`) |
| `present_oem_postprocess` (`none` / `sharpen` / `interpolate` / `both`) | Tells the emulator what the phone already does. Detection would need new JNI reading private OEM settings (STOP item), so the user states it |

`sharpen` (or `both`): no sharpening of ours on top of the phone's. GSR uses the soft variant (edge
sharpness 1.0 instead of 2.0), FSR runs EASU without RCAS (a 1:1 bilinear copy ends the chain), CAS is skipped.

`interpolate` (or `both`): the phone's interpolator doubles a steady 30. `ac6_unlock_probe` is refused (one
warning in the log, `XDROID-AC6T probe 0`), so the cadence stays at the pacer's even 30
(`PaceGuestPresent`). `ac6_slowdown_guard` still works. Any future emulator-side frame generation must check
this too.

Files: `src/xenia/ui/shaders/guest_output_snapdragon_gsr{.xesli,.ps.slang,_soft.ps.slang}`,
`src/xenia/ui/presenter.{h,cc}` (cvars, flow), `src/xenia/ui/vulkan/vulkan_presenter.{h,cc}` (shader
modules; reuses the bilinear pipeline layout and push constants), `xdroid-core/src/main/cpp/xdroid_ac6_timing.cc`.
The presenter edits sit in the upstream tree (no hook point outside it); kept to the effect list and flow.

Checked: both shaders compile to SPIR-V with slangc 2026.8 (the pinned version) using xenia-shader-cc's
flags; C++ passes clang `-fsyntax-only` in an Android configuration. **Build note:** the shader list is a
configure-time GLOB without CONFIGURE_DEPENDS, so re-run the CMake configure (delete the Gradle `.cxx`
cache) or the new bytecode headers won't be generated and the build fails on the missing include.

## 3. Recommended settings to try with Game Manager on

| Case | Settings | Why |
|---|---|---|
| A. Phone interpolates + sharpens (the maintainer today) | `present_oem_postprocess=both`, `present_max_height=1080`, `present_snapdragon_gsr=true` | GSR (soft) upscales 720 -> 1080, the phone adds its sharpening and frames once |
| B. Cheapest for the emulator | `present_oem_postprocess=both`, `present_resolution_scale_percent=50` (720p buffer, 1:1 with the game) | All upscaling and sharpening left to the phone; our present cost drops ~4x |
| C. Phone off | `present_oem_postprocess=none`, `present_max_height=1080`, `present_snapdragon_gsr=true` | GSR at full sharpness |

Change resolution switches before flying: a swapchain recreate is a one-frame gap the OEM interpolator sees.
Compare A vs B on the same flight: `Present stats` gpu_us/paint, screenshots of HUD text (oversharpening
halos) and terrain, and whether the phone's 60 stays smooth.

## 4. Our own 60 FPS reprojection (FG-1), revisited

The phone already interpolates to 60 and the maintainer uses it, so FG-1 drops in priority. It stays parked for:
phones without OEM interpolation, or if the OEM result smears the HUD badly (FG-1 would keep the HUD from the
real frame, which a compositor-level interpolator can't). Never both at once: FG-1 must be refused when
`present_oem_postprocess` says `interpolate`.
