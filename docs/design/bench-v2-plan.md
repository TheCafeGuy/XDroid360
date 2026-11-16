# BENCH-2: GPU-BENCH v2 - hang guard, self-abort, precision, speed, coverage

Status: approved by the maintainer 2026-10-19 (P1 now; P3 upstream guard line OK; guarded repro of the 00005302 hang OK).
P1 written host-only as gpubench_v2.sh + benchab_v2.sh + benchguard.py; replay on the 2026-10-19 logs aborts all 6
hung runs at loop 10 and passes every healthy run. Validated on the phone (section 9): P1 host guard DONE (SHD-2, 4 VALID), P2 in-app guard DONE, C3-C6 stats and energy DONE in benchab.sh.
Still open: build and run C2 live ABBA, E1-E3 suite/soak (written, not run), new traces from the maintainer.
Scope: `tools/device/gpubench.sh`, `tools/device/benchab.sh`, `tools/device/benchsys.py`,
`xdroid-core/src/main/cpp/xdroid_gpu_bench.cc` (+ small hooks, listed per item).

## 0. Evidence from today (2026-10-19, docs/runs/2026-10-19/)

| # | Finding | Evidence |
|---|---------|----------|
| F1 | **Stale results reported as fresh.** vsp2 r1 logs are byte-identical to vsp1 r2 (`vsp2-base-r1.log` = `vsp1-bnd-r2.log`, md5 c465f5f8; `vsp2-dbl-r1.log` = `vsp1-both-r2.log`, md5 92760a87). gpubench polls `grep 'XDROID-BENCH: done' xdroid.log`, which matched the previous session's log before the new app wrote anything; the PNGs share names per trace, so the old ones were hashed too. `vsp2-ab.txt` r1 is therefore wrong. | md5sum of the logs |
| F2 | **Hung runs are scored.** `vsp1-both-r1` and `vsp1-swap0-r1` never wrote `done`; the script waited its fixed 10 min (120 x 5 s), then took PassProf medians (66-69 ms) into the A/B table as if valid. Nothing aborted the app while the phone UI was frozen. | `grep done` empty; vsp1-ab.txt |
| F3 | **The bench hang has the same signature as the maintainer's 11:27 in-game freeze**: `XDROID-BC: STUCK work ...: direct resolve dispatch 4x18x1 (72 groups)`, detail key `00005302 00103000 1C8xx000 00023000 ...` (swap0-r1 frame 603, dest 1C865000; the maintainer-1123 frame 9652, dest 1C888000). The bench can reproduce that path (inferred: heavy/garbage geometry makes the resolve's submission run for seconds; whether the GPU is wedged or only starved is unknown, no kgsl fault in either). | vsp1-swap0-r1.log, the maintainer-1123/xdroid.log |
| F4 | **Runs after a hang are degraded and not flagged.** vsp2-base-r2 wall 46.1 ms vs 40.0 baseline; vsp2-dbl-r2 gpu 24.7 vs ~19; their sys.log is 145 B ("no samples": wrong/empty forensic session picked). vsp1-both-r1: 4.6 W, 74.1 C, Signal_Catcher 21% (ANR stack dumps). | vsp2-ab.txt, vsp1-ab.txt |
| F5 | **Noise is the size of the effects.** Same build/arm "base" flight: 18.31, 18.79, 19.28, 19.34, 19.39, 19.66 ms (spread 1.35 ms, ~7%); effects measured today are 0.3-0.5 ms with 2 rounds. Wall ms/frame is bimodal (p10 28, median 40, max 200-316 ms every run) and is ignored. | *-ab.txt, `XDROID-BENCH: done` lines |
| F6 | No energy per frame: power is a session median of 1 Hz samples, not J/frame per arm; cine arms differ 2093 vs 1567 mW at equal gpu_ms (noise or real, unknown). | s1gc-ab.txt |

Root cause of the UI freezes (inferred, to verify): the bench keeps the GPU 100% busy with submissions of
66+ ms at normal priority; the compositor cannot get GPU time between them, so the UI stalls without any
kgsl fault (fault timeout is not reached). `rest_ms=12` already works around this for cine only.

## 1. Goals (measurable)

1. A bench run can never leave the phone frozen for more than **10 s**: the app aborts itself, the host
   aborts it as backup, and the phone is verified responsive before the next run.
2. Every hang leaves a **dossier** (logs, breadcrumbs, timeline, screencap, sys state) and a ledger row
   with a signature, so repeated hangs group by cause (F3).
3. **No invalid number reaches an A/B table**: stale, aborted, hot or degraded runs are marked INVALID.
4. Precision: report every arm as **delta vs base with a 95% CI**; target CI half-width <= 0.15 ms on
   flight (today ~0.7 ms) and an energy column (mJ/frame).
5. Speed: an A/B of 2 live arms in **one app launch**, stopping early when the CI is decisive.
6. Coverage: a trace suite with golden frame hashes (visual regression) and a soak mode (heat/hangs).

## 2. Part A - hang detection and self-abort (native, in the bench)

- **A1 Heartbeat + bench watchdog thread** (`xdroid_gpu_bench.cc`). BenchThread publishes atomics:
  loop, frame, frame start time, last frame GPU ms. A separate watchdog thread reads them every 100 ms.
  Rule 8: the watchdog takes **no locks** (atomics only), never calls into the command processor.
  Thresholds (cvars, defaults from the warm-up median `w`):
  - SLOW: frame wall > max(4w, 150 ms) -> counter + log line.
  - ABORT: 5 SLOW frames in a row, or any frame > 1 s, or GPU ms median of the last 10 > 2.5w.
  - HANG: no frame finished for 3 s.
  `w` comes from the second warm-up pass, so a broken arm (swap0: 66 ms from frame 1) still trips
  because base `w` is passed by the host as `xdroid_gpu_bench_expect_ms` (from the last valid base run).
- **A2 Abort path.** Set `abort` atomic -> BenchThread stops submitting after the current frame; the
  watchdog logs `XDROID-BENCH: abort reason=<SLOW|HANG|GPU> run=<id> frame=<n>`, calls the existing
  `LogHang()` dump only if the command thread is not the one stuck (it reads mapped memory, no locks:
  verify with xdroid-review before using it from the watchdog), writes `abort.json`, flushes the log,
  then `_exit(3)` after a bounded 2 s. Process death makes kgsl drop the context and its queued work,
  which is what frees the GPU for the compositor (cannot cancel work already in the ring otherwise).
- **A3 Keep the UI alive while measuring: low GPU priority for bench contexts.** Create the bench's
  Vulkan queue with `VK_KHR/EXT_global_priority` = LOW when the driver exposes it (runtime detect, rule 3;
  Turnip-on-kgsl support and kgsl preemption on A750 to verify host-side with the extension list first).
  The compositor then preempts between submits, so even a 66 ms arm cannot freeze the UI. Needs an A/B
  to show GPU ms unchanged when nothing else runs (expected: same). Upstream tree touch = one guarded
  line in device creation -> confirm it is not an "architecture change" (STOP list) with the maintainer.
  Fallback if unsupported: generalize `rest_ms` into an adaptive rest (rest only after frames > 33 ms).
- **A4 Bench-mode coverage of the app wedge killer.** `GameActivity.checkWedged` keys on presents; check
  whether a trace replay presents at all. If not, the app's own killer is blind in bench mode today;
  A1 replaces it there.
- **A5 GPU progress evidence.** In bench mode turn on `vulkan_gpu_breadcrumbs` (cost to measure once:
  if > 1% gpu_ms, only in a `guard=full` mode) and log the completion timeline's completed submission
  every second, so a dossier tells "GPU stuck on work X" from "GPU slow but progressing".

## 3. Part B - host guard (`gpubench.sh`), the backup when the app itself is frozen

- **B1 Run id.** Host generates `run=<name>-<rand>`; passes `xdroid_gpu_bench_run_id`; native echoes it
  on start/progress/done/abort. Host accepts only lines with its id. Before start: truncate xdroid.log
  (`run-as ... : > files/...`) and delete `traces/<trace>.f*.png`. Fixes F1 even before the native part
  (truncate + delete are host-only).
- **B2 Progress watchdog.** Poll every 2 s (cheap grep of the id'd progress line). Deadlines:
  start <= 60 s; then "no new `loops X-Y` line" for > 3 x (expected 10-loop time) + 15 s -> host abort.
  Total cap = 2 x expected run time.
- **B3 Host abort sequence** (order matters, evidence first): screencap -> `logcat -d -b main,system,crash`
  tail 2000 -> pull xdroid.log + forensic sys.log -> `dumpsys gpu` / `/sys/class/kgsl/kgsl-3d0/{gpubusy,
  gpuclk,temp}` snapshot -> `am force-stop` -> wait until `dumpsys SurfaceFlinger` frame stats move again
  (responsive check, 30 s cap) -> cooldown (see C4). `dmesg` needs root on this phone (to check once);
  kgsl faults come from a bugreport (`kgsl_faults.py`), taken only when the signature is new (it costs ~1 min).
- **B4 Dossier + ledger.** `docs/runs/DATE/<run>-hang/` (abort.json, logs, BC lines, png, sys snapshot)
  and one row in `docs/runs/bench-hangs.md`: date, build, trace, arm cvars, reason, signature
  (`STUCK <work kind> key <first detail word>`), frame, temp, verdict. Today's three rows backfilled
  (swap0-r1, both-r1, the maintainer 11:27) under signature `resolve-direct 00005302`.
- **B5 Denylist.** `tools/device/bench_denylist.txt` (cvar=value known to hang: `xdroid_shd_bound_vendian=0`,
  the double-swap probe): gpubench refuses them unless `GUARD=full` (A1-A3 landed) is set.

## 4. Part C - precision

- **C1 Validity gates** (a run is INVALID, printed as such, never averaged): no `done` with our id; abort;
  sys samples < 10; start temp > T0; charging state differs from the A/B's first run; GPU clock residency
  shifted > 15% vs base; frame hashes differ for an arm declared "visual-neutral".
- **C2 In-process interleaved A/B (biggest gain).** One app launch, one warm-up; arms are live cvars
  (`xdroid_live` / live.toml, already used by liveab.py) switched every block of N loops in ABBA order
  (cancels linear drift: heat, clock). 12+ blocks -> paired deltas -> median delta + bootstrap 95% CI.
  Only for live-switchable cvars; build-time or restart-only arms keep the restart path (with C1 gates).
- **C3 Per-frame GPU time** from PassProf into a CSV (frame, gpu_ms, in_pass, between, gpu clock), so
  stats use frames, not one median per run.
- **C4 Thermal protocol, no forcing (rule 3).** Never lock clocks. Before each run/block: wait until the
  max thermal zone < T0 (e.g. 45 C, cvar) with the app stopped; log start/end temp; ABBA cancels the
  remaining drift. Report gpu_ms together with mean GPU MHz per arm.
- **C5 Energy per frame.** Battery mV x mA sampled faster in bench mode (5 Hz if the sysfs read is cheap),
  integrated per block -> mJ/frame per arm with CI; requires unplugged (checked, C1). This is the metric
  the project optimizes now (energy at the 30 FPS cap).
- **C6 Sequential stop.** After each ABBA pair, if CI excludes 0, or CI half-width < 0.1 ms with delta
  inside it, stop. Cap at 30 blocks.

## 5. Part D - speed

- One launch per A/B instead of one per arm x round (C2): today 2 arms x 2 rounds = 4 launches, each with
  boot + 2 warm-up passes + 100 loops + 10 min worst-case wait.
- Poll 2 s instead of 5 s; exit as soon as `done` with our id arrives.
- Hash PNGs on the phone (`md5sum` via run-as), pull only when hashes differ from base.
- Warm pipeline storage is now persisted (93ac8ec1a), so warm-up can drop from 2 passes to 1 when the
  storage reports `Loaded N pipeline descriptions` with N > 0 (to verify on the next run).

## 6. Part E - coverage

- **E1 Trace suite manifest** `tools/device/traces.toml`: name, file, loops, rest policy, golden frame
  hashes per defaults version. Traces: flight, cine (exist), hangar (fix its empty-log bug), menu, a
  resolve-heavy trace of the mission phase with key 00005302 (the maintainer captures it once), loading screen.
- **E2 `benchsuite.sh`**: every trace on one build -> one scoreboard row (gpu ms, mJ/frame, hashes vs
  golden via pngdiff.py). Run before each release and after each default change.
- **E3 Soak mode**: one trace for 10 min with no rest, hot -> hang/thermal/visual-drift detector for
  GFX-0 and "throttle only lowers FPS".

## 7. Order, cost, what each phase needs

| Phase | Items | Build? | Phone | Done when |
|-------|-------|--------|-------|-----------|
| P1 | B1 (host part), B2, B3, B4, B5, C1 | no | 1 normal A/B to validate | stale/hung runs print INVALID; today's ledger rows exist |
| P2 | A1, A2, A4, A5, B1 native id | yes (perf) | 1 guarded repro of a denylisted arm (needs the maintainer's OK) | phone frozen <= 10 s, dossier written |
| P3 | A3 low priority | yes | A/B priority on/off (gpu_ms equal) + guarded repro | UI stays responsive during a 66 ms arm |
| P4 | C2-C6, D | yes | 1 A/B vs the old path on the same arms | CI <= 0.15 ms; one launch per A/B |
| P5 | E1-E3 | no / trace captures | the maintainer captures 1-2 traces | suite row in the scoreboard |

Before P2 and P3: red team (`xdroid-redteam`, lenses Correctness + Platform) on the watchdog/abort and
the priority change. Item order respects "one item at a time": each phase lands and is measured before
the next. The SHD-2 thread keeps the phone until it releases it; P1 is host-only meanwhile.

## 8. Open questions for the maintainer

1. OK to start P1 (host-only) now, and P2-P4 after SHD-2 releases the phone?
2. A3 touches Vulkan device creation in the upstream tree (one guarded line): OK, or treat as STOP item?
3. After P2, may the guard be used to reproduce the `00005302` resolve hang on purpose with a denylisted
   probe (freeze capped at <= 10 s), to open the hunt for the 11:27 freeze?

## 9. Status and results (2026-10-19)

| Item | State | Commit / evidence |
|------|-------|-------------------|
| P1 host guard (B1-B5, C1) | DONE, validated by SHD-2 (v2val: 4 VALID, spread 0.02 ms) | 1c8270743 |
| P2 in-app guard (A1, A2, A5 run id) | DONE on phone: slow abort at loop 3 (80.7 vs 40.4 ms), hang exit on a 4 s frame; aborted run 45 s total, phone answers 1 s after | f77a11fae, nab2 |
| A3 queue priority | CLOSED: high/medium/low same gpu_ms (CI +-0.5 ms, 6 ABBA rounds) and same UI probe (150-190 ms). The UI freeze comes from GPU hangs, not priority | p3-ab.txt |
| A4 wedge killer in bench | not needed: native slow/hang guard covers it | - |
| Hang repro 00005302 | vendian=0 reproduces the maintainer's 11:27 signature in seconds; at high priority it ends in VK_ERROR_DEVICE_LOST (real GPU hang) | rep-hi-r1-hang, bench-hangs.md |
| C2 in-process ABBA (live cvars) | WRITTEN, NDK syntax-checked, not built | 55b2c6a8a, benchlive.sh |
| C3-C6 stats, energy, early stop | DONE in benchab.sh (ABBA rounds, paired 95% CI, ROUNDS=auto, mJ/frame unplugged only) | d13d7a30f |
| Pixel frame compare | DONE: same-code runs differ in 1 px by 4 levels (f3), hash flipped; framecmp.py | d13d7a30f |
| E1-E3 suite, goldens, soak | WRITTEN, not run (benchsuite.sh run / soak) | 55b2c6a8a |
| New traces (menu, explosions, r5302) | needs the maintainer to capture in game | bench_suite.tsv |

Next phone slot (~15 min): build 55b2c6a8a, benchlive.sh with a direct-resolve live cvar, benchsuite.sh run --golden,
benchsuite.sh soak flight.xtr 5. Then the 00005302 hunt can use the guarded repro (vendian=0, GUARD=full).

### Phone slot 2 (2026-10-19 15:38-15:55, build 2196b9b8b)
- Native arms build and switch live (ApplyLiveText). Raw block medians were useless (CI +-9 ms): the profiler reports
  only ~7 of 40 frames per block and the 8 trace frames cost 15-52 ms. Scoring each sample against its trace frame's
  run median fixed it: null arm "same" -0.03 ms, CI +-0.39 (5 rounds x 5 loops). Default BLOCK now 10 loops.
- A fresh replay starts fast (wall ~29 ms/frame) and settles at ~40 after ~15 loops; round 0 is dropped (3+ rounds).
- Suite: flight 18.95 / cine 1.75 / hangar 0.68 gpu ms, goldens saved (docs/runs/golden/); hangar log is fine now.
  Bug fixed: adb ate the suite loop's stdin, so only the first trace ran.
- Soak 5 min flight: 950 loops, no abort, UI max 230 ms; first 10 loops 29.9 -> last 43.5 ms/frame (+45%, heat).
