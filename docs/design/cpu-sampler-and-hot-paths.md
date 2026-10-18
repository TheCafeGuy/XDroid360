# CPU-1: in-emulator CPU sampler + ranked CPU hot-path candidates (2026-10-14)

Cloud analysis, code reading only; nothing here was run on the phone. Numbers marked
"estimate" are inferred from the code, not measured.

## 1. Why a new profiler

The old XDROID-PROF (941c168 / 5498fbc, removed in 4127e58) sampled the guest **LR** of the
running fiber from the scheduler watchdog. It could not see where time goes inside a function,
could not see host time (thunks, kernel exports, LoadClock, MMIO), and could not tell a spinning
thread from a working one except by recognising addresses by hand. simpleperf stays blocked.

## 2. Design (implemented, off by default)

Files: `src/xenia/cpu/guest_sampler.{h,cc}`, hooks in `kernel/guest_scheduler.cc` (RunLoop
registers each dispatch thread), `kernel/xthread.cc` (non-fiber XThreads incl. XHostThreads such
as the CP register at start; `SetCurrentThread` records which guest fiber runs),
`a64_seq_memory.cc` (DELAY_EXECUTION notes the db16cyc address at translation time),
`a64_backend.cc` + `ppc_context.h` (GuestToHostThunk stores its LR in
`PPCContext::host_call_return`).

- **Trigger:** each registered host thread owns a POSIX timer on its own CPU-time clock
  (`CLOCK_THREAD_CPUTIME_ID`, `SIGEV_THREAD_ID`, signal `SIGRTMIN+4`). It only fires while the
  thread burns CPU, so a thread asleep in a wait is never interrupted (no EINTR risk on futex or
  nanosleep paths) and sample counts are proportional to real CPU time. The kernel checks these
  timers on its tick, so the effective rate caps at CONFIG_HZ (250 on most phones).
- **Handler (async-signal-safe):** reads PC and LR from the ucontext, the guest thread id and
  `host_call_return` of the current fiber, and pushes 24 bytes into a per-thread SPSC ring
  (2048 entries). No locks, no allocation, no TLS (the slot comes in `si_value`).
- **Sampler thread:** drains rings every 100 ms; every `guest_sampler_report_s` it resolves
  samples (code cache `LookupFunction` + source map -> guest instruction) and reads each thread's
  CPU clock for its real CPU%.
- **Classification:** `jit` (translated code), `spin` (translated code within 256 bytes of a
  db16cyc in the same function, or inside `guest_sampler_spin_ranges`), `helper` (thunks and JIT
  helpers), `host` (emulator C++ while a guest fiber is current), `host-spin` (host or helper time
  entered from a spin site), `noguest` (scheduler/idle fiber code).
- **Switches:** `guest_sampler_hz` (0 = off, live; 250 recommended), `guest_sampler_report_s`
  (10), `guest_sampler_spin_ranges` (hex ranges, e.g. `"821E6200-821E6C00"` for the AC6 spinlock
  disassembled in docs/runs/2026-10-06/gcode-821E6200.asm).
- **Cost when off:** zero signals; a 1 Hz cvar poll on one sleeping thread; one extra `str` per
  guest->host call (the thunk's LR store, unconditional); one map insert per translated db16cyc.
  **Cost when on (estimate):** one signal per 4 ms of CPU per thread, ~2-5 us each = 0.05-0.1%.
  It does interrupt running guest code, so treat sampler-on runs as forensic, not perf runs.

Output (one block per window), for example:

```
XDROID-SAMPLER 10.0s @250Hz samples=7400 dropped=0 cpu=310% spin cpu=95% work cpu=215%
XDROID-SAMPLER  'CPU 0' cpu=97% samples=2400 spin=57% (work cpu=42%) jit=31% helper=4% host=8% noguest=0%
XDROID-SAMPLER    guest threads: F8000028=88% F80000B8=12%
XDROID-SAMPLER    functions: 821E6200=55.0%[spin] 82374D00=9.1% ...
XDROID-SAMPLER    guest pcs: 821E6AD4=30.1% ...
XDROID-SAMPLER    host: jit-helpers=4.0% libxenia-app.so+0x1A2B3C=3.2% ...
```

Host offsets symbolise offline with `llvm-addr2line -f -e <unstripped .so> 0x1A2B3C` (the
archived build). Validation done in the cloud: arm64 syntax build of every touched file; the
sampler core ran on x86-64 Linux with stubs (busy thread 94-98% CPU, half-busy 46%, idle thread
silent, live on/off works). Not yet run on the phone.

**First PC run:** perf build, `guest_sampler_hz = 250`,
`guest_sampler_spin_ranges = "821E6200-821E6C00"`, mission 1 for 60 s, then once with
`guest_sampler_hz = 0` to confirm FPS is unchanged. Grep `XDROID-SAMPLER`.

## 3. Ranked hot-path candidates

Expected gains are upper bounds until the sampler gives the shares in the "decide with" column.
"Timing" = changes when guest threads observe each other (HUD-1 class races): run HUD-DET too.

| # | Change | Where | Expected gain | Risk | Timing | Decide with |
|---|---|---|---|---|---|---|
| A | Inline mftb: config only, `clock_no_scaling=true` + `clock_source_raw=true` makes LOAD_CLOCK an inline `mrs CNTVCT` + mul/udiv instead of thunk + `UpdateGuestClock` | a64_seq_memory.cc:713; base/clock.cc:95-118 | 0-5% CPU on guest threads (estimate); also removes a global `tick_mutex_` every mftb and the `lock()` path that can sleep | Low (time scalar ignored; nothing in AC6 hooks uses it) | **Yes**, mftb values and spin-loop pacing change | host share of `LoadClock` / `UpdateGuestClock` |
| B | Stack sync off: config only, `a64_enable_host_guest_stack_synchronization=false` drops ~13 instructions (4 loads, 4 stores) per guest call and the post-call check | a64_emitter.cc:1145-1227 | 2-8% of JIT time in call-heavy code (estimate) | Medium: titles that setjmp/longjmp crash; AC6 unknown | No | boot + 1 mission; `jit` share of prologs |
| C | Lean guest->host thunk. Today every host call spills and fills q4-q31 (28 Q regs, 896 B) and writes FPCR twice. Step 1: skip the exit `msr FPCR` when FPCR already equals `fpcr_fpu` (common case). Step 2: save only the vector regs live across the call site (allocator already knows them). Step 3: asm helpers for leaf calls (pattern already used by vrsqrte) | a64_backend.cc:228-351 | Up to the `helper` share; step 1 ~20-40 cycles per call (estimate) | Low (1), medium (2) | No | `helper` + thunk-attributed `host` share |
| D | Fold `mov w0, wN` into `ldr/str [x21, wN, UXTW]` for non-constant addresses when granularity is 4 KB | a64_seq_util.h:303-327 (41 load/store sites) | 1-2% of JIT time (estimate; move elimination may hide it) | Low | No | `jit` share of hot work functions |
| E | Direct `bl rel26` for compiled targets instead of `mov x9, imm64` (4 insns) + `blr` | a64_emitter.cc:607-614 | <2% (P2-J0 never saw dispatch in the top 8) | Low | No | keep demoted |
| F | Critical-section spin under cooperative fibers: `RtlEnterCriticalSection` spins `absolute*256` CAS attempts even when the owner is a fiber parked on the same vCPU, which cannot release until we yield. Skip the spin when the owner is not running on another vCPU | xboxkrnl_rtl.cc:612-620 | Up to its `host` share on contended locks | Medium | **Yes** (waiters block sooner) | host share of `RtlEnterCriticalSection` |
| G | CE-7 spin park (exists, parked: 13x menu glitches) | a64_seq_memory.cc:109-135 | Energy only, up to `spin cpu`; not an FPS lever (the spin waits on CP completion, spin-findings.md §7-12) | Medium | **Yes** | `spin cpu` per window |
| H | Cross-block register allocation / context promotion (passes are per block, so guest regs go back to PPCContext at every block edge) | compiler/passes | Largest structural JIT cost in every Xenia backend; unknown size | High, large | No | only if `jit` work dominates after A-D |

Not candidates: reservations (lwarx/stwcx) are already inline; memory access has no per-access
checks (MMIO via fault handler + recorded sites); lazy flags and SNaN were killed with evidence
(p2-j3-killed.md, P2-F2).

## 4. Suggested order

1. PC: sampler run (above). Pick by numbers: big `helper`/`host` -> C then A; big `jit` in work
   functions -> B then D; big `spin cpu` -> it is CP latency (GPU-side work), not JIT.
2. A and B are config-only A/B runs and can share one session (A with HUD-DET because it is
   timing-changing).
3. C step 1 is a ~10-line change; build it behind the existing thunk with no switch needed
   (bit-identical behaviour), measure with the sampler `helper` share.
