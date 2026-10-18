# XR-3: guest wait engine (design, 2026-10-17; needs approval: architecture change)

The maintainer 2026-10-17: "en vez de simplemente solucionar lo del sleep de cpu, ... un rework entero que aparte de
solucionarlo nos de mas estabilidad y performance?" This replaces CE-7's futex park with one wait model.

## Problem (evidence)
- AC6's main thread spins on a fence the GPU command processor writes back (db16cyc sled,
  a64_seq_memory.cc). Guest_CPU_0 ~91% at the 30 FPS cap on OP12 and S24 (s24-heat.md).
- S24: no OEM frequency cap, so the spin runs the prime core at ~3 GHz: ~7.8 W vs ~4.4 W on OP12 (inferred
  from cur x mV), and the thermal budget is gone by t~600 s (GPU cap 680 -> 353 MHz, 25-28 FPS).
- The ADPF hint counts the spin as work (ADPF-1) unless the db16cyc helper is emitted.
- CE-7 park=1 (futex on the write-back generation, up to 200 us) amplified one-frame menu glitches 18/42 vs
  1/30 (DONE_LOG 2026-10-12). Two flaws, both fixed by design below:
  1. It parks every spin, also spins released by another guest thread (timing change exposes a guest race).
  2. The futex blocks the whole dispatch host thread: co-resident fibers on that guest CPU cannot run, and a
     watchdog preemption cannot land, until the futex times out. The scheduler never sees the wait.

## What already exists (guest_scheduler.h/.cc)
- 6 dispatch threads, guest threads are fibers. Kernel waits park through `BlockCurrentThread`, gated by an
  epoch (object signal epoch, multi-wait summed epoch, or pure delay), targeted wakes (`WakeAll(object)`),
  1 ms poll backoff and a 64 ms backstop. Idle dispatch threads sleep on `ready_event`.
- JIT code can already switch fibers from inside a native call: safepoints call `PreemptCurrentFiber`
  through `guest_to_host_thunk`, with IRQL deferral (no switch at IRQL >= 2 outside user code, bounded).
- Missing piece: guest spin-waits on memory are invisible to the scheduler.

## Design
1. **Classify spin sites** (CE-7b, done in 7d81f4e9f): per guest PC, spins / GPU-released % / time.
   `XDROID-SPIN` every 10 s. A site is GPU-released at >= 64 spins with >= 90% ended by a write-back.
2. **GPU-released spin -> scheduler park** (`guest_spin_park=4`). After `guest_spin_park_hints` hints with no
   write-back, the helper calls a new `GuestScheduler::ParkOnGpuWriteback(seen_generation)`:
   - same rules as the safepoint path (IRQL deferral, terminate check, not inside the global critical region);
   - new gate kind in `RereadyBlocked`: re-ready when `GuestGpuWritebackGeneration() != wait_epoch`;
     deadline backstop 1 ms (a missed bump costs <= 1 ms, never a hang);
   - `WakeGeneration::Bump()` on the GPU thread also pokes CPUs that have a write-back waiter (lock-free
     per-CPU flag + existing `repoll_now`/`ready_event`, no lock on the GPU thread).
   - While parked, the dispatch thread runs other ready fibers or sleeps: CPU really idles, ADPF sees idle.
   - Correctness: the guest loop re-reads its condition after resume, so a wrong wake only costs a loop.
3. **CPU-released spin -> yield only if someone else can run** (`guest_spin_yield=1`). After N hints, call
   `YieldExecution(false)`, which returns without a switch when this fiber is alone on its CPU. Timing is
   unchanged when alone; a co-resident holder (the thread the spin waits for) runs sooner -> stability.
4. **Wake latency counters for kernel waits** (P2-W1, parked since 2026-10-06): per wait kind, signal -> run
   latency p50/p99 in `XDROID-WAIT` every 10 s. Decides whether event/semaphore wakes need work next.
5. **ADPF**: parked time is not CPU, so the 1 s hint and `xdroid_adpf=1` report real work without the
   spin-accounting estimate (kept for CPU-released spins).

## Phases, each a live cvar arm (A/B/A in one flight)
- P0 (this build): CE-7b sites + park=3 (futex, GPU sites only). Data: is the AC6 fence site >= 90% GPU?
- P1: park=4 (scheduler park, GPU sites). Kill tests below.
- P2: guest_spin_yield. P3: XDROID-WAIT counters. P4: defaults switch, hot run, S24 tester run.

## Kill tests (red team before P1 lands)
- Menu glitch rate (glitch recorder, menu cycles by the maintainer) must match park=0 within noise.
- p95 frame time and scheduler overruns no worse than 2x noise vs park=2 (helper without parking).
- Input latency (XDROID-LAT) must not rise (rule: latency only down).
- Energy: CPU % and battery mW at 30 FPS must drop on OP12; S24 gets the tester build for the hot run.

## Expected gain (inferred, not measured)
- Energy: OP12 park=1 measured CPU 296% -> 272% (-8%) with capped clocks; S24 larger (prime core leaves
  3 GHz). FPS: none at the cap; on S24 it delays the thermal GPU cap (25-28 -> 30 FPS for longer).
- Stability: co-resident fibers and watchdog preemptions no longer blocked by a futex park.

## Risks
- Fiber switch from DELAY_EXECUTION, not a safepoint: thunk saves volatile registers like the safepoint
  call; review with xdroid-review JIT checklist (FPCR mode is forgotten by CallNativeSafe).
- A guest that spins with interrupts masked: handled by the IRQL rule (no park at IRQL >= 2).
- Prior art: Canary/Edge spin handling to check before P1 (none ported yet).

## Measured 2026-10-17 (P1 smoke + title live A/B, debug-variant build)
- AC6's spin is one db16cyc sled (site 821E6AE4). Intro: ~80% of spins end after a GPU write-back. Title
  screen: 16-22% (a CPU thread releases it). Learned park (75%) parks in the intro, not on the title.
- Forced scheduler park (park=4, gpu_pct=0) vs helper only (park=2) on the title: CPU -18% (REAL), FPS and
  p95 equal, 45k parks, no hang; mW within noise (short run). docs/runs/2026-10-17/xr3title*.

## P2 prior art (scout, from memory, URLs not fetched; verify before porting)
- Dolphin idle skipping: compile-time match of poll-one-address loops, skips to the next scheduled event.
  Safe only with one emulated CPU; on 360 it can only be a candidate filter, never the wake source.
- RPCS3 SPU: runtime detection of repeated identical reservation reads, then a futex-like wait keyed on the
  address, woken by the reservation write; togglable because some titles regress (missed wakeups).
- yuzu/Ryujinx: guests use WaitForAddress/arbiter SVCs (kernel keyed address lists); raw spins only yield.
- Upstream xenia: db16cyc is only a hint (Canary `delay_via_maybeyield`); no loop detector.
- P2 candidates: (1) confirm a spin by a stable load address+value across hints before parking;
  (2) wake on a keyed guest-address watch (or on the writer fiber's next switch) with the 1 ms backstop
  kept; (3) given the title A/B, first try the simplest P2: park every confirmed spin with GPU wake + backstop.
