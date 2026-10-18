# Turnip driver tuning (GPU-DRV) - research 2026-10-14

Research only (no phone). Every arm below is a candidate for the PC rotation build; none is
measured yet. Gains are estimates from Mesa source reading, not data.

## Where we are

- Reference driver: Turnip **26.2.3** (2026-09-16) via adrenotools. 26.3.0-devel `ec1cc058ad`
  added page faults right after its own 4/128 KB BOs (PLAN.md GPU-DRV, DONE_LOG STAB-DRIVER-1).
- Default `TU_DEBUG=noconcurrentresolves` (xdroid-core `xdroid_driver.cc` SelectVulkanDriver),
  set after STAB-S1/IS-2 located the hang on a7xx concurrent resolves of the 2x MSAA depth RT.
- `tu_debug.txt` in the storage root accepts only `TU_DEBUG`, `IR3_SHADER_DEBUG`,
  `FD_MESA_DEBUG` (`ApplyTuDebugFile`), applied once before the driver loads.
- Our own workaround for a kgsl fence bug: `vulkan_fence_poll_interval_ms=4` and the optional
  `vulkan_async_fence_waiter` (ui/vulkan/vulkan_gpu_completion_timeline.cc), because
  "vkGetFenceStatus sleeps in the kernel (Turnip on kgsl)".
- Already measured, do not repeat: forced `forcebin` and `sysmem` for the whole frame = tie
  (docs/runs/2026-10-04/p1-2b-step0-interim.md). Queue priority is a no-op on kgsl (GPU-PRI);
  `TU_DEBUG=hiprio` does nothing on kgsl either (tu_knl_kgsl.cc sets no context priority).

## What changed upstream since 26.2.3

| Version | a7xx-relevant content | Verdict |
|---|---|---|
| 26.2.4 (2026-10-01) | Nothing for a750 (A702 wide resolve, a650/a660 qrisc hang, ir3 quad votes) | Skip |
| main, MR 44838 (merged 2026-10-08) | `e984ef29` tu/kgsl: 0-timeout waits no longer block forever (kgsl turns timeout 0 into an infinite wait; found as "GPU underutilization"). `1da50a1b` tu/kgsl: correct merge of timestamps across queues and of TS+sync-file | **Our fence bug.** Take it |
| main (26.3.0-devel) | `gmem_warmup` TU_DEBUG flag (new, undocumented); no concurrent-resolve fix found | Wait for 26.3.0 rc |

`e984ef29` explains our workaround: `vkGetFenceStatus` reaches `wait_timestamp_safe()` with a
deadline of "now", the remaining timeout is 0, kgsl waits until the GPU finishes. So every fence
poll on the submitting thread could sleep for a whole GPU submission. That is the same family as
the B1 verdict (b) "CPU waits on host-produced signals".

No upstream fix for our concurrent-resolve hang was found (main has no new
`NO_CONCURRENT_RESOLVES` logic); keep the flag and still file the Mesa report.

## Knobs that already exist in 26.2.3 (verified in mesa-26.2.3 source)

- `TU_AUTOTUNE_ALGO` = `bandwidth` (default) | `profiled` | `profiled_imm` | `prefer_sysmem` |
  `prefer_gmem`. `profiled` times each render pass in both modes and walks a per-pass
  probability to the faster one (locks after >=30% gap, >=1 ms, 30 s stable); `profiled_imm`
  decides after one sample each. Read once per process (`std::call_once`) = per-boot arm.
- `TU_AUTOTUNE_FLAGS` = `big_gmem` (>=10 draws -> GMEM), `tune_small` (tune <=5-draw passes),
  `preempt_optimize` (CP counters PREEMPTION_REACTION_DELAY: passes with 2+ preemption delays
  >1.5 ms in 2 s are forced to GMEM with smaller tiles). Unsupported flags are dropped with a
  warning in the log, so check the log.
- `TU_DEBUG_FILE=<path>`: driver re-reads the file on change (inotify) and applies only runtime
  flags: `nobin sysmem gmem forcebin nolrz nolrzfc perf flushall syncdraw rast_order
  unaligned_store log_skip_gmem_ops 3d_load fdm noconcurrentresolves noconcurrentunresolves
  nobinmerging layout nir`. The folder must exist before launch. This lets the rotation build
  switch driver flags mid-run, no restart. [verify] whether the file replaces or ORs the
  startup `TU_DEBUG`; write `noconcurrentresolves` in every line to be safe.
- Startup-only: `nocb`/`forcecb` (concurrent binning), `noubwc`, `hiprio`, `rd`.

## Rotation arms (priority order)

| Arm | What | How | Expected gain | Hang risk |
|---|---|---|---|---|
| D1 fence fix | Turnip 26.2.3 + cherry-pick `e984ef29` + `1da50a1b` (one file, tu_knl_kgsl.cc). Then cvars A/B: `vulkan_fence_poll_interval_ms` 4 -> 0, `vulkan_async_fence_waiter` false | Build Turnip on the PC (or, fallback, a Banners-Turnip Standard build dated >= 2026-10-08, which is 26.3-devel) | CPU side: the submitting thread stops sleeping in fence polls; less wait on CP, lower energy. Best candidate; size unknown until measured | Low for the backport (kgsl wait path only). Medium for a 26.3-devel build (page faults seen with ec1cc058ad) |
| D2 profiled_imm | `TU_AUTOTUNE_ALGO=profiled_imm` | per boot, needs whitelist (below) | GPU 0-10% per frame (flight GPU is 25-31 ms of a 33 ms budget, so this is the headroom for 60 FPS and energy) | Low-medium: both modes per pass get exercised; watch glitches in passes that switch |
| D3 profiled | `TU_AUTOTUNE_ALGO=profiled` | per boot | Same as D2, adapts over time; more FPS variance (Mesa docs) | Low-medium |
| D4 preempt | `TU_AUTOTUNE_FLAGS=preempt_optimize` | per boot | Fewer long preemption delays: targets STAB-S2 (preemption faults, SystemUI deaths) and hitches. Confirm the flag was not dropped in the log | Low-medium (more GMEM, smaller tiles) |
| D5 perf log | `TU_DEBUG=noconcurrentresolves,perf` | one run (already TU-0 in perf-energy-session.md) | Zero; lists why UBWC/LRZ/GMEM get disabled per pass. Feeds UBWC-1 and the LRZ item | None (log volume) |
| L1 nolrz | live `nolrz` | TU_DEBUG_FILE rotation | Negative or tie on perf; discriminator for GE-1 hangar "barcode" | Low |
| L2 nobinmerging | live `nobinmerging` | TU_DEBUG_FILE rotation | Tie to -3% GPU; discriminator for HUD-1/glitches (bin merging is new a7xx code) | Low |
| L3 nounresolves | live `noconcurrentunresolves` | TU_DEBUG_FILE rotation | Small cost; hang guard if GPU-H2b-like hangs return with concurrent resolves already off | Lowers risk |
| D6 perf build | Banners-Turnip "Perf" (-O3 + ThinLTO, same Mesa source) | swap .zip | Driver CPU time 0-3% | Same as its Mesa base |

Not recommended: 26.2.4 (nothing for a750); concurrent resolves back on (no fix upstream);
whole-frame `sysmem`/`gmem`/`forcebin` (tie, measured); `hiprio` (no-op on kgsl); `big_gmem`
(testing-only per Mesa docs); A7xx/A8xx experimental zips.

## Code needed on the PC (small)

1. `ApplyTuDebugFile`: also accept `TU_AUTOTUNE_ALGO`, `TU_AUTOTUNE_FLAGS`, `TU_DEBUG_FILE`.
2. Rotation build: set `TU_DEBUG_FILE` to a file in the storage root (folder exists at launch);
   the rotator rewrites it per slot (`noconcurrentresolves` + the slot's flag) and logs the slot
   with the frame number, so the black box can attribute GPU ms, glitches and hangs per flag.
3. Log the driver's own lines (`TU_AUTOTUNE`, "Unknown", "unsupported") to confirm each arm took.

## Community builds checked

- The412Banner/Banners-Turnip: plain Mesa main per commit (Standard), Perf (-O3/ThinLTO),
  experimental A710/A720 and A8xx; dropped its two kgsl patches once MR 44838 landed.
- K11MCH1/AdrenoToolsDrivers: 26.0-devel builds with the then-unmerged autotuner overhaul
  (now upstream as `profiled`), CB perf fix (MR 38378, upstream).
- portare-ch/portareos: carries an ARMSX2-only "flush before a declared feedback-loop draw on
  a7xx" patch; engine-specific, not useful for us.
- MrPurple666/purple-turnip, v3kt0r-87, whitebelyash, StevenMXZ: same sources, nothing a750
  specific beyond upstream.

## Sources

- https://docs.mesa3d.org/drivers/freedreno.html (TU_DEBUG, TU_DEBUG_FILE, TU_AUTOTUNE_*)
- https://docs.mesa3d.org/relnotes/26.2.3.html, https://docs.mesa3d.org/relnotes/26.2.4.html
- mesa-26.2.3 `src/freedreno/vulkan/tu_util.cc`, `tu_autotune.cc` (chaotic-cx/mesa-mirror)
- https://github.com/chaotic-cx/mesa-mirror/commit/e984ef29 ,
  https://github.com/chaotic-cx/mesa-mirror/commit/1da50a1b (MR 44838)
- https://github.com/The412Banner/Banners-Turnip (PR #7, releases)
- https://github.com/K11MCH1/AdrenoToolsDrivers/releases
- https://github.com/portare-ch/portareos/pull/612
