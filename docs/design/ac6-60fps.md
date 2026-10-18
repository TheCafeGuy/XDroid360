# AC6 at 60 FPS - what AC6_recomp does, its open issues, and the XDroid360 path

Status: design only (2026-10-13, cloud thread). No phone run behind any number below unless it cites a
run. Inferred numbers are marked *(inferred)*.

Source studied: `sal063/AC6_recomp` (BSD-3-Clause, same license family as us; we already ported its
effect-mode fix as dd714aa6f). Clone at commit `09144bb0`, 172 commits. Their GitHub issue tracker is
not readable from the cloud container, so "known bugs" below come from their fix commits and code
comments, plus the public threads (generationamiga 2026-08-12, neogaf v1.0 thread).

## 1. How the recomp unlocks the frame rate

AC6 gameplay is 30 FPS because the game asks for `D3DPRESENT_INTERVAL_TWO` (swap every 2nd 60 Hz
vblank) and builds its sim step from a measured frame time that is capped at one 30 FPS frame. The
recomp's "unlock" (`ac6_unlock_fps`, master switch, default on) is a set of mid-function hooks on the
US XEX (`src/render_hooks.cpp`, `ac6recomp_config.toml` lines ~10562-10682,
`src/ac6_backend_fixes/ac6_fps_physics_fix.cpp`):

| Hook (guest addr) | What it changes | Commit |
|---|---|---|
| `0x821EFF30` present-interval encoder (r10) | Forces INTERVAL_ONE (r10=1): swap every vblank -> 60 FPS cap | f30af372 |
| `0x821EFD38` delta divisor (r29) | Divisor 60 -> 30 in `delta = min(floor(elapsed*100/(freq/div))+1, 100)`: delta is 100 at 30 FPS and ~50 at 60, so game speed is right | f30af372 |
| `0x821EFD74` delta precision (r8, r10, r29, r30) | Replaces the integer delta with floor(exact + carried remainder): removes the floor+1 bias (+2% speed at 60, +10% at 300). Also raises the cap to `3000/ac6_min_sim_fps` (default 20 FPS -> 150), so the game keeps true speed down to 20 FPS instead of going slow-motion below 30 | d9828e08 |
| same hook, dt-snap | When the measured delta is within ~1 ms of the target, feeds the exact target delta: kills the aircraft "shake" a fixed-step sim shows with sub-ms dt jitter | 6dc41207 |
| wrap `0x823046A0` (force step) and `0x82329B40` (input shaping) | Two flight-model accumulators ([fm+1320] force/speed command, [fm+1456] trigger command) step by fixed per-frame constants with no delta. The wrapper rescales the net per-call change by frame_ms/33.3 (pass-through at 30) | 175480ef |
| `0x82304F40` `fsubs f11,f11,f9` (f9) | Sink-rate drop is applied inside a 1/dt feedback loop, so the glide angle got ~2 deg steeper at 60. Scales f9 by frame_ms/33.3 | fc54d92a |
| entries `0x82184460` (DD) / `0x821856F8` (EM) demo-manager Exec | "Cutscene playing" stamp (100 ms decay). The cinematic Sequencer is frame-locked, so the unlock is suspended in in-engine cutscenes (30 FPS) | c2e2fbfb |
| CP: draw with pixel shader ucode `17e5e4ac3e713245` (world/effects compositor) | "3D world active" stamp (300 ms decay). Menus, hangar and front-end are frame-locked too: guest vblank forced to 60 Hz there; free-run only in the world | 2b52e5e2 |
| `0x821F05F8` before the D3DDevice_Swap branch + VdSwap | Modern pacing: guest vblank free-runs (1000 Hz, gates nothing); `PaceGuestPresent` in VdSwap blocks the game until every earlier swap was delivered by the CP (frame latency 1) plus an absolute-deadline limiter at `ac6_fps_target`. No vblank grid, so a GPU that can only do 45 gives an even 45, not a 60/30 staircase | 6dc41207 |

Ceiling: `ac6_max_sim_fps` = 60 by default ("physics only validated to 60"; code allows up to 240 for
testing). So **the recomp's real maximum is 60 FPS in gameplay**; menus/hangar stay on their native
60 Hz pacing and in-engine cutscenes on 30.

Not part of the unlock but relevant to a slower device: `ac6_cutscene_resync` (b6b94bac) re-syncs
in-engine cutscene video to the audio clock after hitches (bounded extra Exec ticks), and the shader
disk cache fix (97906a6d) stops mission-start pipeline compiles.

## 2. Known issues and weak spots of their approach

Fixed by them (each was a bug the unlock caused first): double-speed cutscenes, double-speed menus,
throttle accel/brake 2x faster, steeper sink/"stronger gravity", +2% game speed from integer delta,
shake from dt jitter, VdSwap asserts, one-frame effect/cloud flicker under host CPU pressure
(9697710e/1d859605, already in our tree).

Still open or fragile (from their code and commits; our assessment):
1. **Only two accumulators audited.** Any other per-frame-constant logic (missile seeker turn, AI
   steering, gun heat, HUD timers/animations, camera shake, wingman command timers) runs 2x fast at 60
   and nobody has reported it yet. Their own note: physics "validated to 60" only. -> We need a 30-vs-60
   behavior diff harness (section 5, P60-4), not trust.
2. **Mode transitions use wall-clock decays** (cutscene 100 ms, world 300 ms). The first cutscene frame
   runs under the unlock; a world fade-out keeps free-running for up to 300 ms. Visible as a short speed
   blip at cuts. Better signal: the demo-manager scheduled flag and an explicit world on/off.
3. **World signal fails closed by shader hash**, and every address is US-XEX specific. Wrong region or
   title update = silently 30 FPS (OK) or, for the address hooks, memory corruption (not OK). We must
   check instruction bytes at every hook address and refuse to install on any mismatch.
4. **`ac6FlipIntervalHook` (0x821EEBE4) never fires** (their own comment); dead weight, do not port.
5. **Frame-time source is the host present timer**, not the guest's measured delta: the physics ratio
   and the game's delta can disagree for a frame after a hitch. Better: compute the ratio from the same
   `exact` delta the precision hook produced this frame (one source of truth).
6. **Their pacer spins the last 2 ms** (`yield` loop): fine on a desktop, an energy cost on a phone.
   Use a deadline `clock_nanosleep` + the existing FP-1 vblank pacing instead.
7. Desktop-only refresh detection (EnumDisplaySettings); on Android we must use the panel mode we vote
   (DISP-2 setFrameRate).

## 3. Mapping to XDroid360 (no new architecture needed)

- **Hook mechanism exists:** `Processor::SetGuestHook` (dd714aa6f) gives kEntry, kReplace and
  kInstruction (mid-function, after a ContextBarrier). That covers every recomp midasm hook. The home is
  `xdroid-core/src/main/cpp/xdroid_ac6_fixes.cc` (title 4E4D07D1, installed from OnLaunch).
- **No jump hooks needed:** with r10=1 at 0x821EFF30 the game's own compare chain reaches the
  INTERVAL_ONE path (verify by disassembling 0x821EFF30..0x821EFF74).
- **Function wrappers (pre/post):** SetGuestHook has no "after" kind. Either (a) kInstruction at the
  single `stfs` that writes [fm+1320] / [fm+1456] (scale new-old in the register before the store; needs
  disasm to confirm one store per call), or (b) kEntry snapshot + kInstruction at each `blr`. Prefer (a).
- **Register write-back:** a kInstruction builtin that edits `ctx->r[]`/`ctx->f[]` is seen by the next
  instruction: OPCODE_CALL_EXTERN is flagged VOLATILE (opcodes.inl:79-82), so ContextPromotionPass
  drops cached context values across it and the next instruction reloads from the context.
- **Pacing:** the C- machinery in VdSwap (`xdroid_vdswap_wait_cp`, `_max_lag 0`) is already the recomp's
  backpressure half. Add the deadline limiter (target from cvar/thermal), the guest vblank override
  (60 Hz in menus/cutscenes, free-run in world), and keep FP-1.
- **World signal:** the cloud de-swizzle fix (f3f9ed5cb) already keys on ucode `17e5e4ac3e713245`; stamp
  a timestamp when a draw binds it.
- **Display:** vote the panel rate per target (DISP-2). On the 120 Hz panel, 30/40/60 all divide 120,
  so a 40 FPS tier paces evenly.
- **Patch files (`.patch.toml`) are not enough:** the delta remainder, dt-snap, physics rescale and mode
  detection are logic, not byte edits. No AC6 file exists in xenia-canary/game-patches (gfx-hunt.md).

## 4. Interaction with HUD-1 (CP timing race, being fixed now)

- The black box shows frame n executing frame n-1's HUD draws while the guest refills the HUD buffer
  (hypothesis ii-b). At 60 FPS the GPU is the bottleneck *by design*, so "CP one swap behind" becomes the
  normal state, and swaps per second double: **expect HUD-1 (and any race of that family) at >= 2x the
  rate.** Shipping 60 before HUD-1 has a root-cause fix would multiply the top visual bug.
- The recomp's pacer *is* C- with max lag 0 (block VdSwap until the CP delivered every earlier swap).
  CW35 measured 0 FPS cost at 30 (hidden in the cap). At 60, GPU-bound, that wait is no longer free;
  measure it, do not assume.
- Upside: a live "unlock" arm (present interval 1 + divisor 30 only) is a **HUD-1 repro accelerator**:
  more swaps, permanent CP lag. Useful for HUD-RUN if marks stay rare. Gameplay feel is irrelevant there.
- The effect-mode race also fires on host stalls; keep `ac6_effect_mode_fix` on (more frames = more
  chances).

## 5. Budget to reach 60 FPS on the OnePlus 12

Frame budget at 60 = 16.7 ms. Target <= 14 ms GPU and CPU so the pacer has slack.

| Resource | Today (mission, 30 FPS cap) | Needed for 60 | Gap |
|---|---|---|---|
| GPU busy / frame | 25.5 ms clean (m5-nocr), 29.6 ms with forensics (hud1-m) | <= 14 ms | **~1.8-2.1x cut** |
| Render passes / frame | ~170 | <= 80 (plan target) | GE-4/GE-3b/GE-1 |
| Guest main thread | ~97% of a core, but much of it is the guest spin-wait (CE-5, P0.3 verdict b) | real work <= 12 ms cool | **unknown: measure first (P60-0)** |
| Prime clock hot | 3187 -> 1132 MHz when SEVERE | CPU work scales ~2.8x hot | sustained 60 hot needs <= ~5 ms cool *(inferred)* |
| Power / thermal | ~4.2 W, SEVERE 80% of play | same W at 2x frames | **energy per frame must halve just to stay as hot as today** |

Reading: menus already run at 60 on the phone (60 FPS menu stretch in hud1-w), so the pipeline itself
can do 60; gameplay is GPU-bound. **Cool phone, 60 FPS needs the GPU plan done (2x); hot phone, 60 is
not realistic this year (inferred) -> adaptive 60/40/30.** A 40 FPS tier needs 25 ms GPU, which is
today's clean number: reachable almost now.

Which perf items matter for 60 (ordered by expected GPU ms; all from PLAN-UNIFICADO 3.2):
1. **XR-3 per-pass GPU profiler** (parked, needs approval): 60 needs a ms-per-pass map; without it
   the 2x is guesswork.
2. **GE-4 / XR-4 prologue splice** (~38 breaks/frame) + **GE-3b** barrier grouping (27/frame).
3. **GE-1 / XR-5 real RT area** (640x2048 -> used height, ~2.8x less load/store per pass on a TBDR).
4. **DRES-COST** (~3 ms measured with breadcrumbs), **UBWC-1** format list, **GE-2/GE-5** load/store ops.
5. CPU only after P60-0 says so: CE-5/CE-7 spin park (frees the core, also energy), CE-3 futex waits.

## 6. Proposed path (each step = live arms + control, per plan rule 13)

| ID | What | Where | Gate |
|---|---|---|---|
| P60-0 | Headroom probe: live cvar `ac6_unlock_probe` = interval 1 + divisor 30 only (no physics fix, never default). One mission cool: FPS, GPU ms, guest main-thread work vs spin per frame | cloud writes, PC runs | after HUD-RUN; doubles as HUD-1 stress arm |
| P60-1 | Port the timing core: present interval, divisor, delta precision with remainder (one source for the physics ratio), dt-snap, min_sim_fps cap. Signature-checked hooks, fail closed | cloud | unit test for kInstruction write-back |
| P60-1b | **Slowdown guard at 30** (same hooks, target 30): true game speed down to 20 FPS when throttled (today 23-27 FPS hot = slow motion). Useful even if 60 never ships | cloud | A/B hot mission, speedrun-timer check |
| P60-2 | Physics: force/trigger accumulators + sink drop (store-site hooks), ratio from the precision hook | cloud | 30-vs-60 flight diff (P60-4) |
| P60-3 | Modes + pacing: cutscene clamp from the demo-manager scheduled flag (no decay), world stamp from the compositor ucode, VdSwap backpressure (reuse C-) + deadline limiter, adaptive target 60/40/30 from thermal headroom, panel vote | cloud | no double-speed cut, even frame times |
| P60-4 | Behavior diff harness: record player/AI/missile state per sim-second at 30 and 60 from the same input replay; flag fields whose per-second change differs > 5% | cloud tool, PC run | finds the bugs the recomp has not |
| P60-5 | Port `ac6_cutscene_resync` (audio-master cutscene catch-up). Independent of 60, helps our hitches | cloud | cutscene with injected stall |
| GPU | Plan items in section 5 order | as planned | GPU <= 14 ms cool |

Order: HUD-1 fix first (section 4) -> P60-0 (measurement, cheap) -> P60-1b (ships value at 30) ->
GPU items -> P60-1..3 behind `ac6_fps_target` (default 30 until the GPU budget is met) -> P60-4 before
any default change.

License: AC6_recomp is BSD-3-Clause; port with attribution (author + hash) like dd714aa6f.

## 7. Built: P60-0 probe + P60-1b slow-motion guard (2026-10-13, off by default)

Code: `xdroid-core/src/main/cpp/xdroid_ac6_timing.cc` (installed from the AC6 on_launch handler only when
the effect-manager vtables match, i.e. the same executable as AC6_recomp). Host syntax check only
(clang -fsyntax-only); not built with the NDK, not run.

| Cvar | Kind | Default | Meaning |
|---|---|---|---|
| `ac6_timing_hooks` | boot | false | Install the hooks (inert while both arms are off) |
| `ac6_unlock_probe` | live | false | P60-0: INTERVAL_ONE + divisor 30 + precise delta + physics scale, in flight only |
| `ac6_slowdown_guard` | live | false | P60-1b: 30 cap kept; true game speed down to `ac6_min_sim_fps` |
| `ac6_min_sim_fps` | live | 20 | 10-30; delta cap = 3000 / value |
| `ac6_dt_snap` | live | true | Exact target step (50 probe / 100 guard) within ~1 ms |

Differences from the recomp: gameplay signal = flight-model step entries (0x823046A0 / 0x82305278), not
a CP shader-hash stamp; physics ratio = this frame's delta / 100; accumulators scaled at their
`stfs ...,1320(rX)` / `stfs ...,1456(rX)` stores found by scanning each function at install (each site is
logged); every fixed site is logged and checked (fsubs word at 0x82304F40, a compare on r10 at
0x821EFF30); no jump hook (r10 = 1 goes through the game's own chain, 0x821EFF30..74 not yet
disassembled: if `interval1` counts rise but FPS stays at 30, that chain is the suspect).

Log: one `XDROID-AC6T` line per 10 s: frames, active frames, mean delta (50 = 60 FPS, 100 = 30, >100 =
guard working below 30), dt-snaps, interval forcings, accumulator stores and sink drops scaled. Install
lines: `AC6 timing sites ...`, `accumulator store <addr> f<n> <off>(r<n>)`, `AC6 timing hooks installed`.

**PC run package (after HUD-RUN closes; one mission, by hand):**
1. Boot with `ac6_timing_hooks = true`. Check the install lines: hooks installed, >= 1 store at 1320 and
   at 1456. If "not installed" or 0 stores, stop and send the log.
2. Live arms, 60 s each, 2 rounds, flight only: `base` (both off) / `probe` (`ac6_unlock_probe=true`) /
   `guard` (`ac6_slowdown_guard=true`). Do not combine with `xdroid_time_coupling`.
3. Read per arm: FPS median/p95, GPU ms per frame, XDROID-SYS per-thread CPU (guest main thread), mean
   delta, HUD-DET detections and user marks (probe = HUD-1 stress), thermal status.
4. By eye in `probe`: throttle up/down feels like 30, glide/sink feels like 30, missiles and AI do not
   look sped up (note anything that does: that is P60-4's list), no double-speed cutscene or menu.
5. `guard` while hot (FPS < 30): game clock and mission timer keep real time (stopwatch vs mission
   timer over 60 s), flight feels normal; `base` hot = slow motion as today.
Kill: crash or hang under an arm, wrong install signature, or `guard` changing anything at a steady 30
(mean delta must stay 100 with snaps ~ frames).
