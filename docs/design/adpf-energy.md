# ADPF-1: per-frame performance hint and thermal tier

Goal: lowest power at a stable 30 FPS on the OnePlus 12 (SD8 Gen 3), with throttling allowed to
lower FPS but never to break frames. This item covers what Android's Dynamic Performance Framework
(ADPF) can do for that, what the code did before, and the switchable implementation in
`xdroid-core/src/main/cpp/xdroid_adpf.cc` (default off).

## 1. What was there

| Piece | Where | Behaviour |
|---|---|---|
| Java hint session | `PerfHint.kt` | 1 s sampler tick; picks the 8 busiest threads every 30 s; reports busiest thread's /proc utime+stime (10 ms ticks) / frames; target 33.3 or 16.7 ms by FPS |
| Thermal | `SystemStats.kt`, `Forensics.kt` | headroom(10 s) and status logged for the HUD/forensics only; nothing acts on them |
| Game Mode / sustained mode | `Forensics.kt` | logged only (`game_mode=`, `sustained_supported=`) |
| CPU pinning | removed (`xdroid_big_core_affinity` gone; prime-core pin is a closed experiment) | the scheduler/EAS places threads |
| Guest spin | `a64_seq_memory.cc` | AC6's main thread spins on a GPU fence (db16cyc sled); `guest_spin_park` (CE-7) can park it, default 0 |

**The problem (inferred from the code, not yet measured):** at the 30 FPS cap the guest main
thread spends most of each frame spinning (Guest CPU 0 ~85%, PLAN CE-5). A spin is CPU time, so the
Java hint reports roughly the whole frame period as "work" whatever the clock is. Above target
-> the power HAL raises clocks; the spin then just spins faster, CPU time stays ~ the period, and
the loop never lets clocks fall. A hint that counts spin as work pushes power up, not down. The
1 s / 10 ms-tick granularity also blurs single frames.

## 2. ADPF facts that matter here

- **Performance Hint** (`APerformanceHint_*`, NDK API 33; `setThreads` 34;
  `setPreferPowerEfficiency` and `reportActualWorkDuration2`/`AWorkDuration` with separate CPU and
  GPU durations 35). Model: a session = the threads that do one recurring piece of work; each cycle
  report the actual duration against a target; the power HAL sizes clocks/uclamp so actual ~
  target. Reports should be per cycle (per frame), not averaged over seconds.
- **Thermal** (`AThermal_*`, NDK 30; `getThermalHeadroom` forecast 31; headroom thresholds 35).
  Headroom 1.0 = SEVERE expected; calling it more than once per second returns NaN. PLAN.md: the
  phone sits in SEVERE ~80% of a mission at the 30 cap (headroom ~1.33), so today the tier logic
  will mostly say 30.
- **Game Mode / Game State** (`GameManager`, API 31/33): the user's per-game mode (standard,
  performance, battery) and `setGameState(loading, ...)`. OEM game modes may apply their own
  interventions unless the app declares `android.game_mode_config` in the manifest. Not
  implemented here (Java + manifest; see section 6).
- **Sustained performance mode** (`Window.setSustainedPerformanceMode`): only on devices that
  report support; Qualcomm/OPlus phones usually do not. Forensics already logs
  `sustained_supported=`; ignore it unless that reads true on the OnePlus 12.

## 3. Design (implemented, off by default)

`xdroid_adpf` (live via `live.toml`):

| Value | Arm | Reported work per frame |
|---|---|---|
| 0 | today (default) | the app's 1 s Java hint runs unchanged |
| 1 | native, spin-free | busiest session thread's CPU time minus its guest spin time |
| 2 | native, raw (control) | busiest session thread's CPU time, spin included |
| 3 | observe (no-hint control) | no session; numbers are computed and logged only |

Any value other than 0 in `live.toml` also closes the Java session (`PerfHint.enabled`), so two
sessions never fight. Set it through `live.toml` (`phone.sh live`/`liveab`), not the config file,
or the Java hint stays on next to the native one.

- **Frame clock:** the command processor bumps `PresentFrameGeneration()` at every guest swap
  (`pm4_command_processor_implement.h`, next to `IncrementFrameNumber`). The ADPF thread sleeps on
  it (futex): one wake per frame, no polling. The bump costs one atomic add when nobody waits.
- **Threads:** names starting with an entry of `xdroid_adpf_threads` (default
  `Guest CPU,GPU Commands`: the 6 guest dispatch threads + the command processor), rescanned every
  5 s; `setThreads` on API 34+, else the session is reopened.
- **Work:** each frame, every session thread's CPU clock (per-thread `clock_gettime`, ns) and its
  spin counter are read. Spin time is charged by the db16cyc helper (`GuestSpinPark`): the gap
  between two hints of one spin (< 50 us apart) is spin; a parked wait sleeps and is never
  charged. Counting runs only while `xdroid_adpf != 0`.
  **The helper exists only when `guest_spin_park` is 1 or 2** (chosen at translation). With
  `guest_spin_park=0` there is no spin counter and arm 1 equals arm 2; run the A/B with
  `guest_spin_park=2` (same code, no parking) to isolate the hint, or `1` to stack both.
- **Target:** frame period tier (60/40/30 from measured swap intervals; a switch needs 30 frames
  in a row) x `xdroid_adpf_target_pct` (default 85). No frame in a wake (loading, pause): nothing
  is reported and the CPU baselines restart, so the next frame does not carry the idle time.
- **Power efficiency:** `xdroid_adpf_power_efficiency=true` calls `setPreferPowerEfficiency`
  (Android 15+), a second energy arm.
- **Thermal tier (advisory):** every 2 s the 10 s headroom forecast sets a cap: >= 0.95 -> 30,
  >= 0.85 -> 40, else 60. Down at once; up only after 30 s below 0.80 (30->40) or 0.70 (40->60).
  Exported as `AdpfRecommendedFpsCap()` and logged; nothing acts on it until the adaptive
  60/40/30 target exists (P60-3 in `ac6-60fps.md`). The device's own thresholds are logged once at
  start (API 35) to calibrate these lines.
- **NDK calls** are resolved with `dlsym` from `libandroid.so` (minSdk 29), so no NDK header
  level is assumed and missing APIs just disable a feature (logged at start).

### 3.1 Audio session (`xdroid_adpf_audio`, live, default 0)

Input from the audio thread: guest thread 18 (AC6's XAudio2 mixer) grows from ~0.8 to ~4.4 ms per
block when hot, suspected stranded on a little core. Under the guest scheduler that guest thread
is a fiber on one "Guest CPU N" host thread, which is already in the frame session, but the frame
session lowers clocks whenever the frame has slack, so it can drag that thread down with it.

`xdroid_adpf_audio=1` (needs `xdroid_adpf` != 0; with 3 it is the only session) opens a second
session: host threads named by `xdroid_adpf_audio_threads` (`Audio Worker,XMA Decoder`) plus the
dispatch thread running guest thread `xdroid_adpf_audio_guest_tid` (set 18 for AC6), re-resolved
every 2 s through the kernel (`DispatchCpuOf(active_cpu)`). Each wake it reports the busiest audio
thread's utilization (CPU minus spin) x `xdroid_adpf_audio_period_us` (5333 = 256 samples at
48 kHz) against `xdroid_adpf_audio_target_pct` (50) of the period: busier than half -> boost.
The dispatch thread also hosts other fibers, so this errs on boosting. A thread in two sessions is
allowed by the API as far as the docs say (not verified on OPlus): `audio_errors` would show it.

Log fields: `audio=` (session open), `audio_threads=`, `audio_busy_p50/max=` (%),
`audio_reports=`, `audio_errors=`, plus an `audio threads ...` line when the set changes.

## 4. Log

Start: `XDROID-ADPF: hint api ... set_threads ... power_efficiency ... thermal headroom ...` and
the thermal thresholds. On changes: mode, thread list, session open, cap steps. Every 10 s:

```
XDROID-ADPF mode=1 fps=30.0 tier=30 target=28.3ms net_p50=.. net_p90=.. net_max=.. raw_p50=..
  spin=..% critical=<thread> threads=7 session=true reports=300 errors=0 pe=false
  headroom=1.12 status=3 cap=30 self_cpu=..ms
```

`net_*` = per-frame work without spin (ms), `raw_p50` = with spin, `spin` = share of the session
threads' CPU spent spinning, `critical` = thread most often on the critical path, `self_cpu` = the
ADPF thread's own cost over the window. `tools/device/liveab.py` now adds `work_ms`, `raw_ms`,
`spin_pct` columns per arm when these lines are present.

## 5. A/B for the PC

One flight, mission, cable out (power numbers are void on USB), `guest_spin_park=2` in the
config (boot) so the spin counter exists without changing behaviour:

```
PE=xdroid_adpf_power_efficiency
phone.sh liveab adpf1 120 3 \
  "A:xdroid_adpf=0,$PE=false" "OBS:xdroid_adpf=3,$PE=false" "RAW:xdroid_adpf=2,$PE=false" \
  "NET:xdroid_adpf=1,$PE=false" "NETPE:xdroid_adpf=1,$PE=true"
```

Every arm names every key: a key an arm leaves out keeps the previous arm's value.

Read: mW, cpu_pct, fps, p95_ms, temp_C per arm (the existing table) plus work_ms/spin_pct.
- Expect (inferred): RAW >= A in mW (it reports the spin per frame, so boosts at least as much);
  NET < A in mW at equal FPS; OBS shows what no hint at all costs.
- **Kill:** NET p95 frame time worse than A beyond 2x noise, or FPS below 29.5 cool -> raise
  `xdroid_adpf_target_pct` to 75 and rerun once; if it still loses, close as Rejected.
- **Keep:** NET (or NETPE) lower mW than A beyond 2x noise with FPS and p95 equal -> make it the
  default and delete the Java hint.
- Second flight if NET wins: `guest_spin_park=1` (park) x NET, since parking removes the spin
  the hint now ignores and the two savings may overlap.
- Check `self_cpu` stays under ~20 ms per 10 s and `errors=0`.
- Audio arm (hot mission, `xdroid_adpf_audio_guest_tid=18` in every arm):
  `"OBS:xdroid_adpf=3,xdroid_adpf_audio=0" "AUD:xdroid_adpf=3,xdroid_adpf_audio=1"`, then
  `"NET:xdroid_adpf=1,xdroid_adpf_audio=0" "NETAUD:xdroid_adpf=1,xdroid_adpf_audio=1"`. Keep it if the
  mixer's per-block time and underruns drop beyond 2x noise for no more than a small mW cost.

## 6. Not done (next steps)

- **GPU duration:** `reportActualWorkDuration2` with `AWorkDuration` (CPU and GPU separately,
  API 35) would let the HAL see GPU-bound frames; needs per-frame GPU time from Vulkan timestamp
  queries on the submit that closes the frame. Do it after the A/B above, if NET wins.
- **Game Mode:** declare `android.game_mode_config` (supports performance + battery modes, no
  OEM downscale/FPS override) and map battery mode -> `power_efficiency=true` and a 30 cap. Needs
  a manifest + Java change; check first whether OPlus applies any intervention today
  (`dumpsys game`).
- **Game State:** `setGameState(isLoading=true)` while no frame is presented (the sampler already
  knows), so the HAL may boost loads and drop afterwards.
- **Thermal tier -> frame target:** wire `AdpfRecommendedFpsCap()` into the P60-3 deadline
  limiter; recalibrate the 0.85/0.95 lines from the logged thresholds and a hot mission.
