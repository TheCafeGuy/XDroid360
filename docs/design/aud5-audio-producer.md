# AUD-5: why audio cuts when hot, and the switches to fix it

Status 2026-10-14: analysis from code, existing logs and a disassembly of the
guest snapshot `scratch/snaps/admm_refinery.bin` (local only). No phone run yet.
All switches below default OFF and are live (`xdroid_live.cc`).

## What thread 18 is

Guest thread 18 (entry 823AD848, guest CPU 4) is AC6's XAudio2 render thread.
Its whole loop (disassembled):

```
823AD89C  KeWaitForSingleObject(event 82916E3C)      ; wait for the kick
          if engine[0x12C] != 0:                       ; a render was requested
            823AD0D8(engine); 823AD1C0(engine, 1)      ; mix one 256-sample frame
823AD8F8  KeSetEvent(event 82916E2C)                   ; "done" (lr 823AD8FC)
```

The guest audio callback (823AD9C0, run by our host audio worker on every
consumed frame) enters the XAudio2 critical section 8268E774, kicks 82916E3C
(lr 823ADA14) and blocks in KeWaitForMultipleObjects({82916E2C, 82916E08})
(lr 823ADA64) until thread 18 is done. Helper render threads (entry 823AD910,
fed by semaphore 82916E18) exist in the code but AC6 creates none
(engine[0x130] = 1), so the whole mix is serial on thread 18.

- The render path (823AD0D8, 823AD1C0 and its direct callees 823AC9E8,
  823ACF20, 823AC7E8) calls no blocking kernel wait: only
  KeRaise/LowerIrql + KeAcquire/ReleaseSpinLockAtRaisedIrql, two
  XAudioGetVoiceCategoryVolume* calls, and virtual calls (voices/effects).
- XMA is not on this path: `use_dedicated_xma_thread` is on, so decoding runs
  on the host XMA worker, and the lock path (`Context*Lock` -> `Block`) is not
  taken per frame.
- "Producer blocked at wait start (1750/1874)" in the ledger is thread 18
  idle at 823AD89C waiting for the kick. That is the design, not a dependency.

## Root cause

The 3.7-4.4 ms the callback "waits" is thread 18's own render time, and it is
CPU-bound. hud1-m (79 ten-second windows, mission 6):

| window quartile | kick -> done | callback mean | calls > 5.33 ms | thread-18 dispatches per render |
|---|---|---|---|---|
| min | 786 us | 1320 us | 8 | 1.1 |
| p25 | 2244 us | 2879 us | 106 | 2.2 |
| p50 | 3971 us | 4408 us | 285 | 5.2 |
| p75 | 4256 us | 4740 us | 366 | 6.1 |
| max | 4600 us | 5165 us | 594 | 7.1 |

Dispatches grow by about one per extra millisecond of render, which is the
1000 us guest timeslice: thread 18 is preempted at quantum end and re-dispatched
at once (ready-to-run mean 1-6 us). Nothing else delays it in the guest. Cool,
the mix takes ~0.8 ms; hot, ~4.4 ms: 5x, far more than the mid-core clock cap
(2131 -> 1401 MHz, 1.5x). The rest is host-side: its dispatch host thread is not
pinned, is not in the ADPF hot set (`PerfHint.kt` takes the busiest threads,
and this one is light), so EAS is free to keep it on a small or contended core.
(Inferred; the new split below measures it.)

The pump is serial (callback n+1 starts after callback n returns), so at a
5.17 ms callback the guest produces 97% of real time with no slack. Every slow
call drains the queue and recovery is slow, so underruns ("+1, producer gap
max 2 ms": the producer is late, not stalled). More buffering cannot fix a
producer at or below real time; concealment only hides it.

## Switches added (all OFF)

| cvar | what | guest timing |
|---|---|---|
| `xdroid_audio_drc_ppm` (try 5000) | Dynamic rate control in the AAudio callback: when the queue is short of full at a device callback, resample up to N ppm slower, scaled by how far behind (full at one burst). The consumer-paced pump follows, so a 97%-real-time producer gets slowed instead of underrunning. Logs `audio drc:` every 10 s. | YES: the guest audio clock runs up to N ppm slower while engaged (0.5% = ~9 cents of pitch). |
| `xdroid_audio_producer_nice` (try -16) | While the producer fiber runs, its dispatch host thread gets this nice value; restored at switch-out. | No ordering change; other fibers on that core get less CPU during the mix. |
| `xdroid_audio_route_watch` | Every 500 ms the service thread checks device id and burst; a route change without a disconnect (BT headphones connecting) resizes the guest queue for the new burst. The existing disconnect path already reopens and resizes. | No. |

Always-on forensic (a few syscalls per producer slice): every 10 s
`audio producer host split per render: N slices, wall X, cpu Y, host runq Z;
slice ends wait/preempt/quantum/yield; cpu us per render by host core; dispatch
nice`, plus one `host core capacities` line.

## How to read the next run (hot mission)

- cpu ~ wall and the cores are small-capacity ones: placement. Fix = put the
  producer's dispatch thread (and the audio worker) in an ADPF session of their
  own (native APerformanceHint, 5.33 ms target per frame) - needs approval if it
  touches JNI.
- host runq large: contention on its core. Try `xdroid_audio_producer_nice=-16`.
- cpu large on big cores too: real guest work grew (e.g. more voices); profile
  the virtual calls in 823ACF20.
- In all cases `xdroid_audio_drc_ppm=5000` should take underruns to ~0 while the
  root fix lands. Kill it if pitch is audible or the cutscene audio sync drifts.

Files: `src/xenia/kernel/guest_scheduler.cc` (split + nice), `xdroid_wait_probe.h`,
`xthread.h` (yield kind), `src/xenia/apu/audio_system.cc` (log),
`xdroid-core/.../xdroid_audio_driver.{h,cc}` (DRC, route watch), `xdroid_live.cc`.
