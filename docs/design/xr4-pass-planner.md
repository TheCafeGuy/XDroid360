# XR-4: render-pass planner (design, 2026-10-15)

Status: **Phase 1 killed by Phase 0 data (2026-10-15, see section 7)**; was design, approved for design by the user 2026-10-15 ("primero diseña xr 4"); code needs
the red team below and the Phase 0 numbers. Architecture change: STOP item, the user approves
each phase that changes recording order.

## 1. Evidence (r6meas, cf3d7eff3, M02 flight, pass profiler 1 frame in 30)

| What | Value |
|---|---|
| GPU per flight frame | ~28 ms (gpu_busy 24-31 ms, 30 FPS cap = 33.3 ms) |
| Passes per frame | 203 = 120 guest + 83 EDRAM transfer |
| Time inside passes | 21.0 ms (guest 18.8, transfer 2.2) |
| Time between passes | **7.1 ms (25%)** |
| Pass ends per frame | resolve 71, rt 46, barrier 38, texload 26, upload 18, submit 4 |
| Costliest pass | main scene 640x2048, 396 draws, 5.7 ms (ends on a barrier) |
| LRZ | Turnip disables it only in transfer passes (depth ALWAYS + stencil write at draw 1); guest passes keep it |

Prior result that bounds this design: GE-4 splice cut passes 232 -> 187 per frame **at equal GPU
time** (DONE_LOG 2026-10-14). Pass count alone is not the cost on this GPU. The original XR-4 goal
("pass count 211 -> <= 80") is therefore dropped as a target; it is only a means.

Working hypothesis (to be confirmed by Phase 0): the 7.1 ms between passes is mostly
serialization, not work. Every resolve records, in order:
1. a shared-memory commit barrier (`VulkanSharedMemory::Use(kComputeWrite)`, previous write range),
2. a source-RT image barrier (attachment -> shader read),
3. the resolve dispatch (direct resolve: 10x90 groups, ~0.03 ms of work),
4. the STAB-S1 post-resolve barrier (COMPUTE -> ALL_COMMANDS, a full GPU drain),
5. on the next draw, the RT back to attachment and shared memory back to draw-read.

71 resolves/frame x 1 full drain each = 71 pipeline drains; on a tiler each drain also flushes
the binning/rendering overlap. Consecutive resolves with no draw between them (sub RP 0x8E: 76
draws, 37 resolves) pay every barrier again although nothing reads in between.

## 2. Goal and exit criteria

- Goal: cut GPU busy per flight frame at the 30 FPS cap with **identical output** (no quality
  change, no skipped work), so the GPU clock and power drop.
- Exit: gpu_busy median down by more than 2x the arm spread in a liveab flight, 0 new hangs in
  3 missions, 0 new glitch marks (GFX-0 recorder + user marks), power (sys.log mW) down.

## 3. Phases

### Phase 0 - measure the gap (no behaviour change; the gate for everything below)
Red team 2026-10-15 (Evidence + Correctness, both LOW on v1): the 7.1 ms is flat across frames
(6.8-7.3 ms while in_pass swings 28-37 ms), which fits a fixed per-boundary cost that may be the
profiler's own stamps or GMEM load/store, not barrier stalls; and GE-4 already removed 45
boundaries for no GPU gain. So Phase 0 must separate these before any code that changes order:
- **Probe cost:** one extra empty timestamp pair per frame (no work between) = cost of a stamp.
- **GMEM split:** stamps inside the pass (after the first draw, before the last) next to the
  outside ones: load/store at pass edges vs the gap itself.
- **Profiler overhead:** gpu_busy with the profiler on vs off, A/B/A in the same scene.
- **Gap by kind:** between-pass ms attributed to the end reason of the pass before it.
- **Resolve runs:** histogram of back-to-back direct resolves that neither clear nor go through
  the scaled mirror or the fallback path (the only ones Phase 1 can touch).
- **Drain price (the kill number):** Δ gpu_busy and Δ mW, `vulkan_direct_resolve_post_barrier`
  off vs on, A/B/A, >= 4 rounds, arm spread < 1 ms, breadcrumbs on (off may hang: accepted for
  this one flight).
- Kill: stop XR-4 if the drain price is < 2 ms, or if probe + GMEM edges explain > 50% of the
  gap, or if the run histogram is mostly length 1. Phase 1 must then predict its gain as
  (drains removed) x (price per drain) and is killed below half of that prediction.

### Phase 1 - lazy post-resolve drain (live switch `xdroid_resolve_drain`)
v1 deferred whole resolves into a batch; the Correctness red team showed that changes results
(the resolve clear, RT `SetUsage` and shared-memory `Use()` state are recorded at call time, the
scaled mirror copies the output, `SubmitRequestedResolve` can end the submission, RTs can be
evicted). v2 defers nothing but the drain:
- Each resolve records exactly as today (its barriers, its dispatch, its state updates, its
  clear), except the STAB-S1 post barrier: it sets `drain_pending_` instead.
- `drain_pending_` is flushed (the same full barrier recorded) before any GPU command that is
  not another direct resolve dispatch: every barrier push, render pass begin, draw, other
  dispatch (dump, clear, transfer, texture load), copy (scaled mirror), submission end, swap.
  One choke point: `SubmitBarriers` and the deferred command buffer's non-dispatch recorders.
- Effect: a run of N back-to-back direct resolves pays 1 drain instead of N; nothing moves.
- Modes: 0 = today (drain after every resolve), 1 = lazy drain, 2 = lazy drain plus a
  compute->compute barrier between resolves in a run (S1 safety comparison).
- Kill on any frame difference vs mode 0 at the same frame (glitch recorder frame hashes) or on
  a hang in 3 missions.

### Phase 2 - pass merge across hoistable work (live switch, after Phase 1 numbers)
- GE-4's prologue splice generalised: uploads and texture loads that end a pass are recorded
  into a prologue before the pass begins; merge when the next pass uses the same RTs and nothing
  between them needs the pass ended.
- Only if Phase 0 shows upload/texload gaps >= 1.5 ms; GE-4 showed fewer passes alone buy
  nothing.

### Phase 3 - barrier hoist (GE-3b)
- Barriers ending the main 396-draw pass (38 barrier ends/frame): find which resource needs
  them (shared memory written by a resolve/memexport earlier in the frame) and move the barrier
  before the pass when the write precedes the pass begin. Needs the Phase 0 barrier census.

## 4. Where it lives
- Phase 0: the pass profiler in `vulkan_command_processor.cc` (XR-3 code) plus resolve-run
  counters in `DirectResolveRenderTargets`.
- Phase 1: `drain_pending_` in `VulkanCommandProcessor`, set by the direct resolve instead of
  its post barrier, flushed at the single non-resolve recording choke point.

## 5. Risks
- S1: a run of resolves now has no drain between them. Mode 2 keeps a compute->compute
  barrier between them; the hang rate decides between 1 and 2.
- A missed flush site would let later work overlap a resolve (the S1 condition): the flush sits
  in the recording choke point, and a counter logs flushes per site to show none is missing.
- Results cannot change: no command is reordered or skipped, only a full barrier is merged.

## 6. Fit with the CE perf arms (same build, same flight)
- CPU arms (log_idle_block, guest_scheduler_quantum_us 1000/2000/4000, xdroid_adpf 0/1,
  guest_spin_sleep_us 0/50) do not touch GPU recording: they rotate in the same liveab flight
  as Phase 0, measured on process CPU % and mW (sys.log), not on GPU busy.
- The post-barrier price arm is Phase 0 itself.
- Build 1 = Phase 0 instrumentation + the 4 CPU arms; flight 1 rotates them. Build 2 = Phase 1
  (if Phase 0 clears the kill bar) with the CPU arms that won flipped to default.

## 7. Phase 0 result (r7rot, 0df991cce, 2026-10-15, ~10 min rotation, ended by an S1 hang)
- Resolve runs per flight frame: 71 single mergeable resolves, ~1 run of 2, ~18 not mergeable
  (clear / scaled / fallback). The lazy drain would merge about 1 drain per frame: Phase 1 killed.
- Drain price: arm `nodrain` (post-resolve barrier off) gpu 30.2 ms vs base 29.2 (spread 1.1):
  no measurable cost. The S1 barrier stays.
- Probe (empty stamp pair) 0.4 us: the stamps are not the gap. Gap after each end reason
  (ms/frame): texload 2.75, resolve 2.35, barrier 2.1, upload 0.1-0.3, rt 0.02. The 7 ms is real
  work recorded between passes, led by texture loads (~29/frame in flight).
- Next GPU lever (TEX-1, not XR-4): why ~29 texture loads per flight frame - which textures are
  re-loaded every frame (invalidated by resolves or CPU writes?) and whether the loads can be
  skipped when the guest data did not change.
