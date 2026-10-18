# Intense perf/energy session plan (PES-1, drafted 2026-10-11)

Goal: same 30 FPS in AC6 missions with less energy and heat, plus the perf bugs that waste work.
Format of the 2026-10-11 bug hunt: scout fan-out, many atomic commits behind cvars, red team on
the risky ones, one grouped phone session at the end. Hard rule 1 still applies: every claim
of a win needs a device number (power in mW, GPU busy, process CPU), not only FPS.

Targeted scoreboard rows (PLAN.md section 1): thermal SEVERE share (80% -> < 10%), GPU busy
ms/frame (26 -> <= 18), render passes/frame (211 -> <= 80), process CPU (2.59 -> <= 1.7 cores),
plus a new row: **battery power at the 30 FPS cap (mW)**, which has never been measured.

## Evidence gathered while planning (2026-10-11, build e2d226ec5, mission loaded)

- **Display path, measured on the phone:**
  - The game's SurfaceView layer is composited by SurfaceFlinger on the GPU (`Comp Type CLIENT`,
    `Transform ROT_90`, source crop 3168x1440 on a 1440x3168 portrait panel).
  - Cause: `vulkan_presenter.cc:1066` `TODO(Triang3l): Support transforms`, and :1258 always
    picks `preTransform = IDENTITY` even though the surface reports ROTATE_90. The display
    processor cannot rotate our RGBA buffer, so RenderEngine rotates and copies 4.5 MP on the
    same Adreno for every frame we present.
  - That RenderEngine work runs on a higher-priority GPU ring and preempts our long submissions
    every frame. Preemption faults of this kind were STAB-GPUHANG-4.
- **Refresh rate, measured:**
  - The panel runs at 120 Hz (`mActiveRenderFrameRate=120`; modes are 60/90/120) while our
    layer presents 29.9 frames/s (`--latency`, 126 frames in 4.18 s).
  - Nothing in app or core calls `setFrameRate`, and the layer's explicit frame-rate vote is
    empty.
- **Presents follow guest swaps 1:1** (`presenter.cc:442-460`), so there is no over-presenting.
  MAILBOX is picked when offered (`vulkan_presenter.cc:1298-1314`).
- **Overlay work:**
  - A full-size ComposeView sits on top of the SurfaceView, and stats are on by default
    (`ingame_show_stats=true`); the stats text is redrawn every 1 s.
  - The 1 s sampler also runs `forensics.sample`, `perfHint.update` and a full /proc thread walk
    (every 1 s while the overlay is visible, otherwise every 10 s): `GameActivity.kt:350-412`.
- **Render passes:**
  - Every guest pass begins with `renderArea` = the full EDRAM-height host RT (640x2048) and uses
    LOAD+STORE for color and depth (cp:2994-3017, rtc:1648-1748).
  - Pass ends come from 13 site groups (list: scout report in this file's history). The main
    ones are `SubmitBarriers()` ending the pass whenever any barrier is pending (cp:2899), texture
    loads (tc:1740/1913/1994), shared-memory uploads (sm:667), resolves and transfers.
  - No per-reason counts exist yet (GE-0).
- **Turnip bandwidth savers:**
  - Color RTs whose ownership-transfer format differs get `MUTABLE_FORMAT` with no
    `VkImageFormatListCreateInfo` (rtc:1990), which disables UBWC on Turnip. Texture-cache images
    do chain a list (tc:1201).
  - LRZ is likely lost on pipelines that write FragDepth or use kill/demote.
  - Both points are inferred from Mesa rules; `TU_DEBUG=perf` confirms them without a build.
- **Guest spin:**
  - The fence counter at 0xFF068000 is plain memory written back by the CP (no MMIO read path).
  - The AC6 spin loop already executes `db16cyc`, which the A64 JIT lowers via
    OPCODE_DELAY_EXECUTION. `guest_spin_sleep_us` (a64_seq_memory.cc:36) is a blind sleep on
    that hint, off by default.
  - The CP is ~100% busy in wall time, so parking the spin saves energy, not FPS.
- **Upstream:** ~17 perf-relevant edge commits are unported.
  - Clean: f453ede2e and a0d11bec9 (JIT precompile, 1 GB cache), 98b6319c5 (watch re-arm skip),
    54729c29f (timers on the 1 ms tick), 3e9d62ffc (WAIT_IB_PFD_COMPLETE).
  - Manual merge: 6435e7990 (multi-wait blocks; our threading_posix already has its own
    generation scheme, so port only the futex wait), 935e03876 (resolve copy outside the global
    lock).
  - Deferred: the async-pipeline series (06fb359de, e8c05cd05, 8a4d405af, c8503efe6, eec245698,
    562e06ff9) is high-risk and only matters while shaders compile.

## Phase 0 - measure first (host build, then the first phone block)

| ID | What | Why |
|---|---|---|
| PWR-0 | `tools/device/power.sh N`: 1 Hz battery current x voltage, thermal zones, kgsl gpuclk/gpubusy, cpufreq per cluster; median/p95 mW | No energy number exists. **Runs must be wireless with the cable out**: on USB the charger feeds the phone, and bypass charging zeroes current_now |
| GE-0 | Pass-end reason counters + passes begun (guest/transfer) in the Frame stats line | Decides between GE-3/GE-4/barrier work by numbers |
| TU-0 | One run with `TU_DEBUG=perf` (tu_debug file, no build): grep ubwc/lrz/sysmem reasons | Confirms or kills UBWC-1 and the LRZ item for free |
| BASE | Mission 5, 3 x 60 s, battery > 50%, phone cooled to the same start temperature, stats overlay as shipped | The reference every A/B is judged against |

Free data point from today's GPU-H2 test: if runaway SPIR-V loops also ran in frames that did not
hang, GPU busy/frame should drop from ~26 ms. Compare Frame stats against the 2026-10-08 mission logs.

## Phase 1 - verified quick wins (evidence above; each behind a cvar or setting)

1. **DISP-1 Vulkan pre-rotation:**
   - Create the swapchain with `preTransform = currentTransform` and the extent swapped for 90/270.
   - Rotate in the final paint: the guest-output blit and the ImGui drawer, via a rotation in the
     vertex stage of the presenter's paint pipelines.
   - Recreate on transform change.
   - Expect `Comp Type DEVICE`, no RenderEngine work on the GPU, and none of its per-frame
     preemption of our submissions.
   - Prior art: the Android "Vulkan pre-rotation" guide (Apache-2.0 sample).
   - This fills an upstream TODO: keep it Android-guarded and record it in
     UPSTREAM_IMPROVEMENTS.md.
   - Risk: medium (orientation flips, 180-degree landscape, ImGui hit-testing coordinates).
2. **DISP-2 frame-rate vote:**
   - Call `Surface.setFrameRate(rate, FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)` on the SurfaceView
     surface (API 30+; the change strategy needs API 31+).
   - Derive the rate from the measured guest present rate (demand-driven, nothing hardcoded) and
     update it when the rate changes (menus 60, missions 30).
   - Expect the panel at 60 Hz instead of 120. Verify with `dumpsys display`.
3. **DISP-4 overlay:** set the ComposeView to GONE while the menu is closed and stats are off,
   and default `ingame_show_stats` to off for players (keep it on for test builds). Gate the 1 s
   thread walk and `forensics.sample` on actual need (CE-6).
4. **DISP-5 present mode A/B:** `vulkan_allow_present_mode_mailbox=false` (FIFO), cvar only.
   Pairs with DISP-2 for a stable cadence. Watch input latency.
5. **UBWC-1:** chain `VkImageFormatListCreateInfo {format, transfer_format}` on mutable color RTs
   (rtc:1990), mirroring tc:1201. Only if TU-0 shows UBWC disabled for them.
6. **Clean ports:** f453ede2e, a0d11bec9 (JIT stutter, FP-2), 98b6319c5 (mprotect churn, FP-4),
   3e9d62ffc. 54729c29f goes behind a measurement because it changes timer wake cadence.

## Phase 2 - GPU energy core (needs the GE-0 histogram)

- **GE-4 prologue splice:**
  - Record texture loads and shared-memory uploads ahead of the pass instead of ending it,
    using a per-page pass epoch so a page written inside the pass still forces a break.
  - Largest expected pass reduction: PLAN estimated 40-80 breaks/frame.
- **GE-3b barrier hoist:**
  - `SubmitBarriers(false)` ends the pass for any pending barrier (cp:2899).
  - Barriers that do not touch the bound attachments can be batched before the next pass begin.
  - Fold resolve clears into the next guest pass (GE-3).
- **GE-2 / GE-5 load-store ops:** LOAD_OP_CLEAR with a clear-rect area for clear passes, and depth
  storeOp DONT_CARE when a later full clear makes the stored depth dead.
- **GE-1 real RT height:**
  - Build the outside-area write detector first: the hangar "barcode" broke the scissor-union
    variant.
  - Then shrink the render area to the max Y written: ~2.8x less area per pass on a tiler.
- **LRZ-1:** count pipelines that write FragDepth or kill with depth write. Where the Xenos state
  allows it, keep early-Z (e.g. alpha test without depth export).

## Phase 3 - CPU energy

- **CE-7 spin park (new):**
  - In `GuestSpinPause`, after N `db16cyc` hints with no progress, park on a "CP progress"
    generation (atomic + futex, no lock: hard rule 8).
  - The CP bumps the generation after MEM_WRITE / EVENT_WRITE* write-backs.
  - Bounded timeout of 100-500 us, so spins that wait on CPU peers still progress.
  - Step 0 (temp): an AC6-only guest hook on the spin at 821E6AC8, to measure the energy ceiling
    before building the generic version.
  - First identify which PM4 write-back hits 0xFF068000 (one log line).
- **CE-3 port:** take only the futex wait from 6435e7990 and keep our generation and refcount
  logic and the lock-free realtime path.
- **CE-4 rest:** logger disruptor spin, GuestScheduler watchdog while paused, the 100 ms quit poll.
- **CE-5:** retire `guest_spin_sleep_us` if CE-7 lands, since it is a blind version of CE-7.

## Phase 4 - perf bugs

- **P6-DEGRADE:** FPS sinks over a mission and terrain textures blink. With PWR-0 we can split
  thermal throttling (gpuclk falling) from texture-cache thrash (TEXMEM eviction lines).
- **FP-1:** verify vblank pacing (MarkVblank 3600+-2 per 60 s; mission locked at 30, not 29.3).
- **FP-3:** 935e03876, resolve copy outside the global lock (manual merge; helps audio and p95).
- **ASYNC-1:** shader stand-ins on first play. Judge from the second pass of today's test before
  deciding whether the async-pipeline series is worth its risk.

## Ambitious reworks (design doc + approval first: architecture change is a STOP item)

| ID | System | Leverage | Cost | Exit criterion |
|---|---|---|---|---|
| XR-3 | GPU frame profiler: timestamps outside passes, 1 frame in N, top-K pass signatures (RT format/size/draws/reason); optional `VK_KHR_performance_query` counters if Turnip exposes them | Turns every GPU energy item into a measured one; today we only have total busy | ~1 session | Per-pass ms that sum to within 10% of gpubusy |
| XR-4 | Render-pass planner: a per-submission recorder that hoists uploads, texture loads, barriers and clears ahead of passes and merges adjacent passes on the same RTs (GE-3 + GE-4 as one system) | Pass count 211 -> <= 80 | 2-3 sessions | Pass count and GPU busy down, zero new glitches on the GFX-0 marks |
| XR-5 | Real render-target extents: write-extent tracking per RT (draw scissor + resolve/transfer/clear rects + memexport), area and later allocation sized to it (GE-1 done right) | ~2.8x less load/store area per pass | 1-2 sessions | Detector reports 0 outside-area writes for a full mission; hangar hexagon intact |
| XR-6 | Display path rework: pre-rotation + frame-rate vote + optional reduced swapchain size with display-processor scaling (`setFixedSize`) | GPU composition gone, panel 60 Hz; with the reduced size, fewer pixels in the final paint | 1 session | DEVICE composition, 60 Hz in missions, mW down by more than 2x noise |
| XR-7 | Generic idle-loop parking in the JIT (idea level only from other emulators): recognise load-compare-delay loops with no stores, park them like CE-7 | CPU energy on every title, not just AC6 | 2 sessions | Process CPU down with no FPS loss across 3 titles |

The reduced swapchain size in XR-6 changes image quality (scaler choice), so it is a user
setting, never a silent default.

## Order and phone blocks

1. **Host block A:** PWR-0, GE-0, DISP-1, DISP-2, DISP-4, UBWC-1 (gated), clean ports, CE-7
   step 0 (temp). Red team DISP-1 and CE-7.
2. **Phone block 1** (wireless, cable out, ~40 min):
   - BASE 3 x 60 s.
   - Group 1: DISP-1 + DISP-2 + DISP-4. They are independent mechanisms, so bisect only if the
     group regresses.
   - DISP-5 alone.
   - TU-0 run.
   - CE-7 step 0.
   - Then BASE again (A/B/A).
3. **Host block B:** GE-4 / GE-3b / GE-2 chosen by the histogram; CE-7 generic; CE-3; FP-3.
4. **Phone block 2:** A/B/A with power, pass count and GFX-0 glitch marks.
5. **XR-3 .. XR-7** as separate approved projects, one at a time.

Not retried (closed experiments, xdroid-perf skill): WFE yield, prime pin, forced clocks, PGO,
mid-frame or submit splits, texture budgets, zero-copy, fast-forward, SNaN elision, audio-nop.

## Bugs captured in play (2026-10-11 20:48 session, build db8da6eed, breadcrumbs on)

The user marked three glitches in mission 5 (`docs/runs/2026-10-11/f204822/`, marks at 20:51:08,
20:51:54, 20:52:13; bugreport-f204822.zip). kgsl: 0 hangs, 0 faults, 0 preemptions in the
session, so none is a STAB-S1 GPU hang.

| Bug | Evidence | Cause | Solution in this plan |
|---|---|---|---|
| PB-1 frame drops in flight (marks 2, 3) | FPS 25-28 at t=207-213 and t=226-230 s; kgsl busy ~100% (busy/total 1.00); gpu_busy/frame 31.1 ms in flight vs 26.8 ms in m5a without breadcrumbs (+16%); p95 frame 50 ms | The GPU has ~20% headroom at the 30 FPS cap; the breadcrumb writes (forensic builds only) push it over, and any heavier scene does the same for players | Breadcrumbs stay off outside forensic runs. Real fix = GPU energy core: DISP-1 removes SurfaceFlinger's per-frame GPU rotation, GE-0 histogram picks GE-4/GE-3b/GE-2 (pass count 170/frame in flight) |
| PB-2 audio crackle (mark 1) | ~140 underruns in 4 min; producer gap max 153 ms in flight, 133 ms in menus; a burst of 5-7 underruns at frames 7676-7707 right before mark 1; the AUD-1 queue resize (ee8d119b6) did not remove them | The guest callback does not deliver frames for 70-150 ms; unknown whether the callback itself runs long (blocked in guest code) or the worker does not call it (AUD-2) | Build 1 logs `audio pump N calls: guest callback max X ms, gap between calls max Y ms` once per second when either exceeds 20/30 ms. Next step by outcome: long callback -> sample the guest PC/wait object during it; long gap with short callbacks -> the consumer-paced slot release (apu_consumer_paced) |
| PB-3 objects missing on first appearance | 13 "Draw skipped (pipeline not ready yet)" in 2 bursts (f:7295-7302, f:9690), down from 121 in m5a: pipeline storage works, new effects still compile on first sight | ASYNC-1 (async_shader_skip_draws) | Unchanged in build 1; the residue shrinks as storage fills. Revisit ASYNC-1 only if bursts stay after 3 sessions |
| PB-4 loud clipped audio at a transition | f:9690: rms 0.41, peak 2.06, 6841 samples limited, same frame as PB-3's second burst | Likely the mission-end sting mixing above 1.0; our limiter clamps it | Observe; compare with Xendroid on the same transition before treating it as a bug |

## Build 1 (single build, 2026-10-11 night): what is in it

All new behaviour sits behind cvars or app switches; the live ones change mid-flight through
`files/xdroid/live.toml` (`phone.sh live`, `phone.sh liveab`), so one flight measures every arm.

| Item | Commit | Switch (default) | Live |
|---|---|---|---|
| Ports f453ede2e, a0d11bec9, 98b6319c5 | 69217e203, 1ef0eeed4, 30e9910b5 | always | - |
| 3e9d62ffc | already in the tree | - | - |
| 54729c29f timer tick | ce2c06973 | `timer_console_tick` (false) | yes |
| GE-0 pass-end reasons | 7fb4f5a8f | always ("Pass ends/frame:" line) | - |
| DISP-1 pre-rotation | d39c57804 | `present_prerotate` (true), `present_prerotate_flip` (false) | yes |
| DISP-2 frame-rate vote | app | `xdroid_frame_rate_vote` (true) | yes |
| DISP-4 overlay A/B | app | `xdroid_show_stats` (menu setting) | yes |
| DISP-5 FIFO | existing cvars | `vulkan_allow_present_mode_mailbox/_fifo_relaxed` | yes |
| CE-7 spin park | 7c55715cd | `guest_spin_park` (0; 1 park, 2 helper only), `_hints` 32, `_timeout_us` 200 | 1<->2 yes |
| UBWC-1 format list | build 1 | `vulkan_render_target_format_list` (false) | no (RT creation) |
| PB-2 audio pump report | build 1 | always (rate-limited) | - |
| PWR-0 + live A/B tools | 35b02583b | `phone.sh power`, `phone.sh liveab`, `liveab.py` | - |

Red team (xdroid-critic, Correctness): DISP-1 MEDIUM, proceed (kill: image not upright in both
landscape orientations with flip=0, or VUIDs); CE-7 MEDIUM, proceed (kill: p95 frame time or
scheduler overruns worse than 2x noise vs `guest_spin_park=2`).

## Phone session 1 (replaces phone block 1; one sitting, two boots)

Boot 1 (perf, cable OUT, battery > 50%, breadcrumbs off, `guest_spin_park=1` so the helper is
compiled in):
1. Check the picture is upright; turn the phone 180 degrees once and check again (DISP-1 kill test).
2. Mission 5, flying. Then `phone.sh liveab pes1 45 3` with arms
   - `base`: prerotate false, vote false, park 2, stats as shipped
   - `disp`: prerotate true, vote true, park 2
   - `park`: disp + park 1
   - `fifo`: park + mailbox false, fifo_relaxed false
   - `tick`: park + timer_console_tick true
   - `nostats`: park + xdroid_show_stats false
   = 6 arms x 45 s x 3 rounds, about 14 min of flight. The tester only flies.
3. Verdicts from liveab.py (2x the same-arm spread); composition and refresh rate per segment in
   `pes1-display.txt` (DISP-1 expects DEVICE composition, DISP-2 expects 60 Hz).

Boot 2 (forensic, after boot 1): `vulkan_gpu_breadcrumbs=true` plus `tu_debug.txt = perf`
(TU-0). Fly until a GPU hang or 15 min (STAB-S1 run 1). Then bugreport, kgsl_faults.py, the
stall report against kgsl events, and grep logcat for ubwc/lrz/sysmem. UBWC-1
(`vulkan_render_target_format_list=true`) is enabled only if TU-0 shows UBWC disabled on the
mutable color render targets; it needs its own boot pair because it acts at RT creation.

## Session 1, attempt 1 (2026-10-11 21:36-21:59, build ff91fb370) - what happened

- **Picture froze at the first live switch** (base1, present_prerotate=false; user mark 21:39:16):
  the presenter returned an outdated connection and asked the UI thread to reconnect, but on
  Android that request was a Java invalidate of a SurfaceView with no draw hook, so it was
  dropped; the game kept running. The user's Home + return rebuilt the swapchain (f:30040).
  Fixed in 815cf4058 (paint requests run on the native UI looper). The same bug would have
  frozen the picture on a 180-degree turn with pre-rotation on.
- **Stray process:** TaskStop killed only the wrapper; the liveab child kept switching arms.
  Lesson: kill the child processes too (`ps -ef | grep liveab`).
- **Power data:** the shell sampler never ran (OnePlus denies the battery nodes to shell; Git
  Bash also rewrote the /data and /sys paths). The app's sys.log has cur (mA here), mv, plug,
  hz and cpu per second; liveab.py now uses it (034e3e088).
- **Partial safe A/B** (one segment per arm, battery 29.6 -> 33 C, flight, prerotate on):
  4.1-4.3 W total; base drifted +146 mW between rounds. CE-7 park: CPU 296% -> 272% (-8%,
  REAL against a 10-point spread), -119 mW (noise). Stats overlay off: -255 mW (noise, 2x
  noise = 292). Frame-rate vote: no effect because it voted 29 (panel stayed 120 Hz; fixed
  875caedcb). p95 33.7 ms in every arm.

User glitch marks (f213601/glitch-marks.txt) mapped to the log:

| Mark | Context | Log | Cause / fix |
|---|---|---|---|
| 21:38:04 | menu, 60 FPS | nothing | visual only; unknown |
| 21:39:16 | base1 live switch | no new swapchain | presenter stuck -> 815cf4058 |
| 21:52:37 | flight | 50 ms frame, new pipeline compiled | PB-3 shader first use |
| 21:52:56, 21:54:31, 21:54:54 | flight | bursts of 5-12 audio underruns | PB-2 -> 451062873 |
| 21:54:25 | flight | 1-2 underruns, 38 ms frame | minor |
| 21:54:41 | flight | 17 underruns, 66 ms frame + new pipeline | PB-2 + PB-3 |

PB-2 answered by the pump report: guest callback 3-10 ms per 5.33 ms frame (not blocked),
calls ~60 ms apart = the 40 ms power_saving bursts; the queue held exactly 2 bursts, so
clustered slow callbacks emptied it with the producer only 2-13 ms late (328 underruns in
22 min). apu_queue_bursts=3 adds a burst of margin; the callback cost itself is AUD-2's lead.

GE-0 in flight (pass ends/frame): resolve 53, rt 30, barrier 27, upload 20 (p90 40), texload
18, submit 4 (about 152 of 170). GE-4 (texload + upload, ~38/frame) is mapped
(prologue splice before the pass begin; only `tight_area_args_offset_` stores a stream
offset; needs per-pass read sets for shared-memory pages and a pass epoch on textures, and
the post-load barriers must go in the prologue too). It is a GPU-core change with glitch risk:
design + red team before code, after this session's numbers.
